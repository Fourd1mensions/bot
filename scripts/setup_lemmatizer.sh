#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
USER="$(whoami)"

echo "Setting up lemmatizer service..."
echo "  User: $USER"
echo "  Script dir: $SCRIPT_DIR"

# Create venv if missing
if [ ! -d "$SCRIPT_DIR/venv" ]; then
    if ! python3 -m venv --help >/dev/null 2>&1; then
        echo "Error: python3-venv not installed. Run: sudo apt install python3-venv"
        exit 1
    fi
    echo "Creating virtual environment..."
    python3 -m venv "$SCRIPT_DIR/venv"
    "$SCRIPT_DIR/venv/bin/pip" install pymorphy3 || "$SCRIPT_DIR/venv/bin/pip" install pymorphy2
fi

# Generate service file with current user/paths
SERVICE_FILE="/tmp/lemmatizer.service"
cat > "$SERVICE_FILE" <<EOF
[Unit]
Description=Pymorphy2 Lemmatizer Service
After=network.target

[Service]
Type=simple
User=$USER
WorkingDirectory=$SCRIPT_DIR
ExecStart=$SCRIPT_DIR/venv/bin/python3 $SCRIPT_DIR/lemmatizer_service.py --socket /tmp/lemmatizer.sock
Restart=on-failure
RestartSec=5

# Cleanup socket on stop
ExecStopPost=/bin/rm -f /tmp/lemmatizer.sock

# Security hardening
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=read-only
PrivateTmp=false
ReadWritePaths=/tmp

[Install]
WantedBy=multi-user.target
EOF

# Install service (requires sudo)
if ! sudo -n true 2>/dev/null; then
    echo "sudo access required to install systemd service"
fi
sudo cp "$SERVICE_FILE" /etc/systemd/system/lemmatizer.service
sudo systemctl daemon-reload
sudo systemctl enable lemmatizer.service
sudo systemctl restart lemmatizer.service

echo "Done. Status:"
systemctl status lemmatizer.service --no-pager
