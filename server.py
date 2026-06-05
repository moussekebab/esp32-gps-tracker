"""
GPS Tracker — UDP Server
Runs on Windows, requires Python 3.x (no extra libraries needed)

What it does:
  - Listens for UDP packets on PORT
  - Expects payload: "lat,lng"  e.g. "48.858844,2.294351"
  - Appends each point to data.json
  - data.json is what the webapp reads

Usage:
  python server.py

Requirements:
  - Python 3.x (https://python.org)
  - Open UDP port in Windows Firewall (see bottom of this file)
"""

import socket
import json
import os
from datetime import datetime, timezone

# ─── Config ──────────────────────────────────────────────────────────────────

HOST = "0.0.0.0"   # listen on all interfaces
PORT = 4210        # must match SERVER_PORT in the ESP code
DATA_FILE = "data.json"

# ─── Init data file ───────────────────────────────────────────────────────────

def load_data():
    if os.path.exists(DATA_FILE):
        with open(DATA_FILE, "r") as f:
            try:
                return json.load(f)
            except json.JSONDecodeError:
                pass
    return {"points": []}

def save_data(data):
    with open(DATA_FILE, "w") as f:
        json.dump(data, f, indent=2)

# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    data = load_data()
    print(f"GPS Tracker server starting on UDP port {PORT}")
    print(f"Storing points to: {os.path.abspath(DATA_FILE)}")
    print("Waiting for packets...\n")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((HOST, PORT))

    while True:
        try:
            raw, addr = sock.recvfrom(64)   # packets are tiny (~20 bytes)
            payload = raw.decode("utf-8").strip()
            print(f"[{datetime.now().strftime('%H:%M:%S')}] Received from {addr[0]}: {payload}")

            # Parse "lat,lng"
            parts = payload.split(",")
            if len(parts) != 2:
                print(f"  -> Ignored: unexpected format")
                continue

            lat = float(parts[0])
            lng = float(parts[1])

            # Basic sanity check
            if not (-90 <= lat <= 90) or not (-180 <= lng <= 180):
                print(f"  -> Ignored: coordinates out of range")
                continue

            # Append point with UTC timestamp
            point = {
                "lat": lat,
                "lng": lng,
                "ts": datetime.now(timezone.utc).isoformat()
            }
            data["points"].append(point)
            save_data(data)
            print(f"  -> Saved: {lat}, {lng}")

        except ValueError:
            print(f"  -> Ignored: could not parse '{payload}'")
        except KeyboardInterrupt:
            print("\nStopped.")
            break

    sock.close()

if __name__ == "__main__":
    main()


# ─── Windows Firewall ─────────────────────────────────────────────────────────
#
# Run this once in an Admin PowerShell to open the UDP port:
#
#   netsh advfirewall firewall add rule name="GPS Tracker UDP" ^
#     protocol=UDP dir=in localport=4210 action=allow
#
# To remove it later:
#   netsh advfirewall firewall delete rule name="GPS Tracker UDP"
