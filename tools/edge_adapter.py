import json
import time
from datetime import datetime, timezone
from typing import Any

import requests
import serial

# Configuration
SERIAL_PORT = "COM3"         # Verify its the actual port plugged
BAUD_RATE = 115200
INGEST_URL = "https://telemetry-audit-engine.onrender.com/api/v1/nodes/cold_room_compressor_01/ingest"

def post_to_auditron(payload: dict[str, Any]) -> None:
    """Send a telemetry payload to Auditron"""
    try:
        response = requests.post(INGEST_URL, json=payload, timeout=5)
        if response.status_code == 202:
            print(f"[OK] {response.json().get('message', 'Accepted')}")
        else:
            print(f"[ERR] {response.status_code}: {response.text[:200]}")
    except requests.exceptions.RequestException as e:
        print(f"[EXC] {e}")
        time.sleep(1)

def main():
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
    except serial.SerialException as e:
        print(f"Failed to open {SERIAL_PORT}: {e}")
        print("Check that the ESP32 is connected and the port is correct.")
        return
    print(f"Connected to {SERIAL_PORT}. Listening for telemetry...")
    buffer = ""

    while True:
        try:
            if ser.in_waiting > 0:
                chunk = ser.read(ser.in_waiting).decode("utf-8", errors="ignore")

                buffer += chunk

                # Process complete lines (JSON ends with newline)
                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    line = line.strip()
                    if not line:
                        continue

                    try:
                        data = json.loads(line)
                    except json.JSONDecodeError:
                        print(f"[WARN] Malformed JSON: {line[:100]}")
                        continue

                    # Replace timestamp with current UTC time
                    data["timestamp"] = datetime.now(timezone.utc).isoformat()

                    print(f"[LOG] {json.dumps(data)}")
                    post_to_auditron(data)
            time.sleep(0.01)
        except KeyboardInterrupt:
            print("Stopped by user.")
            ser.close()
            break

if __name__ == "__main__":
    main()