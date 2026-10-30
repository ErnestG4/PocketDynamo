#!/usr/bin/env bash
# Build and flash env pd in one go. The running firmware's USB is TinyUSB
# (serial + MIDI), so first ask it over serial ('U') to reboot into ROM
# download mode, wait for the bootloader's USB-Serial/JTAG port, then upload.
#   tools/flash.sh [env] [port]
set -euo pipefail
ENV=${1:-pd}
PORT=${2:-/dev/ttyACM0}
cd "$(dirname "$0")/.."
pio run -e "$ENV"
product() { for d in /sys/bus/usb/devices/*; do [ -f "$d/idVendor" ] && [ "$(cat "$d/idVendor")" = 303a ] && cat "$d/product" 2>/dev/null; done; }
# (No `product | grep -q` here: under pipefail grep's early exit SIGPIPEs
# the loop and the test reads false.)
if [[ "$(product)" == *PocketDynamo* ]]; then
  python3 - "$PORT" <<'PY'
import serial, sys
s = serial.Serial(); s.port = sys.argv[1]; s.baudrate = 115200; s.dtr = True; s.rts = True; s.write_timeout = 1
s.open(); s.write(b'U'); s.flush()
PY
  # The bootloader's port shows up within milliseconds, but through usbipd
  # it takes a few seconds before it opens: retry until it does.
  # First the old port has to go: until then an open succeeds on it.
  for _ in $(seq 1 50); do [[ "$(product)" == *PocketDynamo* ]] || break; sleep 0.1; done
  python3 - "$PORT" <<'PY'
import serial, sys, time
for _ in range(100):
    time.sleep(0.25)
    try:
        serial.Serial(sys.argv[1]).close(); break
    except Exception:
        pass
time.sleep(0.5)
PY
fi
pio run -e "$ENV" -t upload --upload-port "$PORT"
