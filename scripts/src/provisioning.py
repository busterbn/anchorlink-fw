#!/usr/bin/env python3
"""
Automates feeding TLS credential shell commands via a serial port.
Place your PEM files (key.pem, cert.pem, ca.pem) in the same directory as this script,
then run:

    python3 cred_import.py --port /dev/tty.YOUR_PORT --baudrate 115200

You can adjust:
  --tag      the SecTag for your client cert/key (default 1)
  --ca-tag   the SecTag for your CA cert (default 2)
"""
import serial
import time
import argparse
import os
import base64
from cryptography import x509
from cryptography.hazmat.primitives import serialization

script_dir = os.path.dirname(os.path.abspath(__file__))

def wait_for_prompt(ser, timeout=3):
    end_time = time.time() + timeout
    while time.time() < end_time:
        line = ser.readline().decode(errors="ignore")
        if 'uart:~$' in line:
            return True
    return False

def send_commands(ser, commands, delay=0.1):
    """
    Send a list of commands over the serial port, pausing 'delay' seconds between each.
    """
    for cmd in commands:
        # Clear any previous input from the device
        ser.reset_input_buffer()
        # Log the command being sent
        print(f"> {cmd}")
        # Send the command
        ser.write((cmd + '\r\n').encode())
        time.sleep(delay)
        # Read and print complete lines from the device
        while ser.in_waiting:
            line = ser.readline().decode('utf-8', errors='ignore').rstrip()
            print(line)

def import_der_base64(ser, filename, sectag, ctype, chunk_size=63):
    """
    Convert PEM to DER, base64-encode DER, split into 63-char chunks,
    send via cred buf, then commit with cred add ... BIN.
    """
    # clear buffer
    send_commands(ser, ["cred buf clear"], 0.2)

    # load PEM and convert to DER
    pem_path = os.path.join(script_dir, filename)
    with open(pem_path, "rb") as f:
        pem_data = f.read()
    if ctype in ("CA", "SERV"):
        cert = x509.load_pem_x509_certificate(pem_data)
        der_bytes = cert.public_bytes(serialization.Encoding.DER)
    elif ctype == "PK":
        key = serialization.load_pem_private_key(pem_data, password=None)
        der_bytes = key.private_bytes(
            encoding=serialization.Encoding.DER,
            format=serialization.PrivateFormat.PKCS8,
            encryption_algorithm=serialization.NoEncryption()
        )
    else:
        raise ValueError(f"Unsupported credential type: {ctype}")

    # base64-encode DER
    b64_data = base64.b64encode(der_bytes).decode("ascii")

    # send chunks
    for i in range(0, len(b64_data), chunk_size):
        chunk = b64_data[i:i + chunk_size]
        send_commands(ser, [f'cred buf "{chunk}"'], 0.05)
        time.sleep(0.05)

    # commit using BIN (no null terminator)
    send_commands(ser, [f"cred add {sectag} {ctype} DEFAULT BIN"], 0.2)

def main():
    parser = argparse.ArgumentParser(
        description="Import TLS credentials via the Zephyr shell over serial."
    )
    parser.add_argument(
        "--port",
        default="/dev/tty.usbmodem0008210085151",
        help="Serial port (default /dev/tty.usbmodem0008210085151)"
    )
    parser.add_argument(
        "--baudrate", type=int, default=115200,
        help="Serial baudrate (default 115200)"
    )
    parser.add_argument(
        "--device-id",
        type=str,
        required=True,
        help="Device ID (3 characters)"
    )
    parser.add_argument(
        "--level-sensor-ok",
        type=str,
        required=True,
        help="01 for on and 00 for off"
    )
    parser.add_argument(
        "--debug-mode",
        type=str,
        required=True,
        help="01 for on and 00 for off"
    )
    args = parser.parse_args()

    # Validate device ID length
    if len(args.device_id) != 9:
        parser.error("--device-id must be a 3-character string")

    # Open serial port
    with serial.Serial(args.port, args.baudrate, timeout=1) as ser:
        # give the board time to reset and present the shell prompt
        time.sleep(2)

        wait_for_prompt(ser)
        send_commands(ser, [f''], 0.5)
        # Write level sensor ok flag
        wait_for_prompt(ser)
        # if args.level_sensor_ok is not None:
        #     if args.level_sensor_ok == "1":
        #         send_commands(ser, [f'settings write hex device/level_sensor_ok 01'], 0.5)
        #     else:
        #         send_commands(ser, [f'settings write hex device/level_sensor_ok 00'], 0.5)
        # else:
        #     send_commands(ser, [f'settings write hex device/level_sensor_ok 00'], 0.5)

        send_commands(ser, [f'settings write hex device/level_sensor_ok {args.level_sensor_ok}'], 0.5)

        # Write debug mode
        wait_for_prompt(ser)
        # if args.debug_mode is not None:
        #     if args.debug_mode == "1":
        #         send_commands(ser, [f'settings write hex device/debug_mode 01'], 0.5)
        #     else:
        #         send_commands(ser, [f'settings write hex device/debug_mode 00'], 0.5)
        # else:
        #     send_commands(ser, [f'settings write hex device/debug_mode 00'], 0.5)

        send_commands(ser, [f'settings write hex device/debug_mode {args.debug_mode}'], 0.5)

        # Write device id
        wait_for_prompt(ser)
        send_commands(ser, [f'settings write string device/id {args.device_id}'], 0.5)


        # Write certificates
        wait_for_prompt(ser)
        import_der_base64(ser, "../certificates/mqtt-001-key.pem", 1, "PK")
        wait_for_prompt(ser)
        import_der_base64(ser, "../certificates/mqtt-001-cert.pem", 1, "SERV")
        wait_for_prompt(ser)
        import_der_base64(ser, "../certificates/broker-cert.pem", 1, "CA")

        # Write provisioned ok flag
        wait_for_prompt(ser)
        send_commands(ser, [f'settings write hex device/provisioned_ok 01'], 0.5)

        # Finally, list stored credentials
        wait_for_prompt(ser)
        send_commands(ser, ["cred list"], 0.5)

if __name__ == "__main__":
    main()