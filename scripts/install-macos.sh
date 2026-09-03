#!/bin/bash
set -euo pipefail

INSTALL_DIR="/usr/local/bin"
CONFIG_DIR="/usr/local/etc/mistserver"
LOG_DIR="/usr/local/var/log/mistserver"
PLIST_PATH="/Library/LaunchDaemons/com.ddvtech.mistserver.plist"
OLD_PLIST="$HOME/Library/LaunchAgents/com.ddvtech.mistserver.plist"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ "$(id -u)" -ne 0 ]; then
  echo "This script must be run as root (sudo ./install.sh)"
  exit 1
fi

if ! ls "$SCRIPT_DIR"/Mist* >/dev/null 2>&1; then
  echo "Error: No MistServer binaries found in $SCRIPT_DIR"
  exit 1
fi

# Check for Homebrew-managed MistServer
if command -v brew >/dev/null 2>&1 && brew services list 2>/dev/null | grep -q mistserver; then
  echo "Warning: MistServer is already managed by Homebrew."
  echo "Installing here will create a separate copy at $INSTALL_DIR."
  echo "The Homebrew version at $(brew --prefix)/bin/ will remain."
  read -rp "Continue? [y/N] " confirm
  [[ "$confirm" =~ ^[Yy] ]] || exit 0
fi

# Install binaries
echo "Installing MistServer to $INSTALL_DIR..."
mkdir -p "$INSTALL_DIR"
INSTALLED=0
for f in "$SCRIPT_DIR"/Mist*; do
  [ -f "$f" ] && [ -x "$f" ] || continue
  cp "$f" "$INSTALL_DIR/"
  INSTALLED=$((INSTALLED + 1))
done
echo "Installed $INSTALLED binaries."

# If MistController is already running, hot-reload and exit
if pgrep -x MistController >/dev/null 2>&1; then
  if kill -s USR1 $(pgrep -ox MistController); then
    echo "Update complete."
  else
    echo "Update installed, but MistServer will have to be manually restarted."
  fi
  exit 0
fi

# Create config and log directories
mkdir -p "$CONFIG_DIR"
mkdir -p "$LOG_DIR"

# Install MistTray.app if bundled
if [ -d "$SCRIPT_DIR/MistTray/MistTray.app" ]; then
  echo "Installing MistTray.app to /Applications..."
  cp -R "$SCRIPT_DIR/MistTray/MistTray.app" /Applications/
  echo "MistTray installed"
elif [ -d "$SCRIPT_DIR/MistTray.app" ]; then
  echo "Installing MistTray.app to /Applications..."
  cp -R "$SCRIPT_DIR/MistTray.app" /Applications/
  echo "MistTray installed"
fi

# Migrate: unload old user-level LaunchAgent if present
if [ -f "$OLD_PLIST" ]; then
  echo "Migrating from user-level LaunchAgent..."
  launchctl unload "$OLD_PLIST" 2>/dev/null || true
  rm -f "$OLD_PLIST"
fi

# Set up LaunchDaemon
echo ""
echo "Setting up MistServer service..."
launchctl unload "$PLIST_PATH" 2>/dev/null || true

cat > "$PLIST_PATH" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>com.ddvtech.mistserver</string>
  <key>ProgramArguments</key>
  <array>
    <string>$INSTALL_DIR/MistController</string>
    <string>-c</string>
    <string>$CONFIG_DIR/config.json</string>
  </array>
  <key>WorkingDirectory</key>
  <string>$CONFIG_DIR</string>
  <key>KeepAlive</key>
  <true/>
  <key>StandardOutPath</key>
  <string>$LOG_DIR/mistserver.log</string>
  <key>StandardErrorPath</key>
  <string>$LOG_DIR/mistserver.err.log</string>
</dict>
</plist>
EOF

launchctl load "$PLIST_PATH"
echo "MistServer service started (auto-starts at boot)"

# Open MistTray for the console user if applicable
CONSOLE_USER=$(stat -f "%Su" /dev/console 2>/dev/null || true)
if [ -n "$CONSOLE_USER" ] && [ "$CONSOLE_USER" != "root" ] && [ -d "/Applications/MistTray.app" ]; then
  CONSOLE_UID=$(id -u "$CONSOLE_USER")
  echo "Starting MistTray..."
  launchctl asuser "$CONSOLE_UID" open -a MistTray 2>/dev/null || true
fi

echo ""
echo "MistServer installed successfully."
echo "Config:        $CONFIG_DIR/config.json"
echo "Logs:          $LOG_DIR/mistserver.log"
echo "Web interface: http://localhost:4242"
echo ""
echo "To uninstall:"
echo "  sudo launchctl unload $PLIST_PATH"
echo "  sudo rm $PLIST_PATH"
echo "  sudo rm $INSTALL_DIR/Mist*"
echo "  sudo rm -rf $CONFIG_DIR"
echo "  sudo rm -rf $LOG_DIR"
