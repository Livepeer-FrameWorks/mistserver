#!/bin/bash
set -euo pipefail

INSTALL_DIR="/usr/bin"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ "$(id -u)" -ne 0 ]; then
  echo "This script must be run as root (sudo ./install-linux.sh)"
  exit 1
fi

if ! ls "$SCRIPT_DIR"/Mist* >/dev/null 2>&1; then
  echo "Error: No MistServer binaries found in $SCRIPT_DIR"
  exit 1
fi

echo "Installing MistServer binaries to $INSTALL_DIR..."
INSTALLED=0
for f in "$SCRIPT_DIR"/Mist*; do
  [ -f "$f" ] && [ -x "$f" ] || continue
  cp "$f" "$INSTALL_DIR/"
  INSTALLED=$((INSTALLED + 1))
done
echo "Installed $INSTALLED binaries."

if [ -d "$SCRIPT_DIR/MistTray" ]; then
  echo "Installing MistTray..."
  cp -r "$SCRIPT_DIR/MistTray"/* "$INSTALL_DIR/" 2>/dev/null || true
elif [ -f "$SCRIPT_DIR/MistTray" ] && [ -x "$SCRIPT_DIR/MistTray" ]; then
  cp "$SCRIPT_DIR/MistTray" "$INSTALL_DIR/"
fi

if pgrep MistController >/dev/null 2>&1; then
  if kill -s USR1 $(pgrep -o MistController); then
    echo "Update complete."
  else
    echo "Update installed, but MistServer will have to be manually restarted."
  fi
  exit 0
fi

LINE=$(stat /proc/1/exe 2>/dev/null | head -n1 | cut -d'>' -f2 || true)

if echo "$LINE" | grep -q systemd; then
  echo "Detected systemd."
  cat > /etc/systemd/system/mistserver.service << 'EOF'
[Unit]
Description=MistServer
After=network.target

[Service]
Type=simple
Restart=always
RestartSec=2
TasksMax=infinity
TimeoutStopSec=8
Environment=MIST_LOG_SYSTEMD=1
ExecStart=/usr/bin/MistController -c /etc/mistserver.conf
ExecStopPost=/bin/bash -c "rm -f /dev/shm/*Mst*"
ExecReload=/bin/kill -s USR1 $MAINPID

[Install]
WantedBy=multi-user.target
EOF
  systemctl daemon-reload
  systemctl enable mistserver
  echo "Successfully installed MistServer using systemd. Starting MistServer."
  systemctl start mistserver
else
  echo "Detected init."
  cat > /etc/init.d/mistserver << 'INITEOF'
#! /bin/sh
### BEGIN INIT INFO
# Provides:          mistserver
# Required-Start:    $network
# Default-Start:    2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: MistServer service
# Description:      MistServer - next generation multimedia server
### END INIT INFO

NAME=MistController
DESC="MistServer"
CFG_FILE=/etc/mistserver.conf
LOG_FILE=/var/log/mistserver.log

do_start()
{
  echo "Starting $DESC" "$NAME"
  MistController -L $LOG_FILE -c $CFG_FILE &
  RETVAL=$?
  [ $RETVAL = 0 ] && echo "Success" || echo "Failure"
  return $RETVAL
}

do_stop()
{
  echo "Stopping $DESC" "$NAME"
  kill `pidof MistController` >/dev/null 2>&1
  RETVAL=$?
  [ $RETVAL = 0 ] && echo "Success" || echo "Failure"
  return $RETVAL
}

do_restart()
{
  do_stop
  sleep 10
  do_start
  return $RETVAL
}

case "$1" in
  start)
    do_start
    ;;
  stop)
    do_stop
    ;;
  restart)
    do_restart
    ;;
  *)
    echo "Usage: $SCRIPTNAME {start|stop|restart}" >&2
    exit 3
    ;;
esac
INITEOF
  chmod 755 /etc/init.d/mistserver
  if command -v chkconfig >/dev/null 2>&1; then
    chkconfig mistserver on
    echo "Successfully installed MistServer using init. Starting MistServer."
    service mistserver start
  elif command -v update-rc.d >/dev/null 2>&1; then
    update-rc.d mistserver start 42 2 3 4 5 . stop 42 0 1 6 .
    update-rc.d mistserver defaults
    echo "Successfully installed MistServer using upstart (init). Starting MistServer."
    service mistserver start
  else
    echo "Could not register service for auto-start. Starting MistServer manually."
    /etc/init.d/mistserver start
  fi
fi

echo ""
echo "MistServer installed successfully."
echo "Web interface: http://localhost:4242"
