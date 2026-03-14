import subprocess
import sys
import tempfile
import os

# nRF5340 APP core UICR addresses
UICR_CUSTOMER_BASE = 0x00FF8080

def check_low_voltage(stderr):
    """Check if error output indicates low voltage issue"""
    if stderr and 'low voltage' in stderr.lower():
        return True
    return False

def read_raw():
    result = subprocess.run(['nrfjprog', '--memrd', f'0x{UICR_CUSTOMER_BASE:08X}', '--n', '16'],
                          capture_output=True, text=True)
    print(result.stdout)

def write_serial(serial_str, debug_mode=None):
    parts = serial_str.split('-')
    ccc = int(parts[0])
    ppp = int(parts[1])
    ssssss = int(parts[2])

    result = subprocess.run(['nrfjprog', '--recover'], capture_output=True, text=True)
    if result.returncode != 0:
        if check_low_voltage(result.stderr):
            print("ERROR: Low voltage detected on device")
        else:
            print(f"Error during recover: {result.stderr}")
        sys.exit(1)

    # Prepare all data (16 bytes total)
    data = bytearray(16)
    data[0:4] = ccc.to_bytes(4, 'little')
    data[4:8] = ppp.to_bytes(4, 'little')
    data[8:12] = ssssss.to_bytes(4, 'little')

    if debug_mode is not None:
        val = 1 if debug_mode else 0
        data[12:16] = val.to_bytes(4, 'little')
    else:
        data[12:16] = b'\xFF\xFF\xFF\xFF'

    # Write hex file
    with tempfile.NamedTemporaryFile(mode='w', suffix='.hex', delete=False) as f:
        hex_file = f.name

        # Extended linear address record for 0x00FF0000
        upper_addr = (UICR_CUSTOMER_BASE >> 16) & 0xFFFF
        checksum = (-(0x02 + 0x00 + 0x00 + 0x04 + (upper_addr >> 8) + (upper_addr & 0xFF)) & 0xFF)
        f.write(f':02000004{upper_addr:04X}{checksum:02X}\n')

        addr = UICR_CUSTOMER_BASE
        for offset in range(0, 16, 16):
            chunk = data[offset:offset+16]
            line_addr = (addr + offset) & 0xFFFF
            line = f':{len(chunk):02X}{line_addr:04X}00'
            checksum = len(chunk) + (line_addr >> 8) + (line_addr & 0xFF)
            for b in chunk:
                line += f'{b:02X}'
                checksum += b
            checksum = (-(checksum & 0xFF)) & 0xFF
            line += f'{checksum:02X}\n'
            f.write(line)

        f.write(':00000001FF\n')

    try:
        result = subprocess.run(['nrfjprog', '--program', hex_file, '--sectorerase'],
                              capture_output=True, text=True)
        if result.returncode != 0:
            if check_low_voltage(result.stderr):
                print("ERROR: Low voltage detected on device")
            else:
                print(f"Error programming UICR: {result.stderr}")
            sys.exit(1)
    finally:
        os.unlink(hex_file)

    result = subprocess.run(['nrfjprog', '--reset'], capture_output=True, text=True)
    if result.returncode != 0:
        if check_low_voltage(result.stderr):
            print("ERROR: Low voltage detected on device")
        else:
            print(f"Error during reset: {result.stderr}")
        sys.exit(1)

    print(f"Written: {ccc:03d}-{ppp:03d}-{ssssss:06d}")
    if debug_mode is not None:
        print(f"Debug mode: {debug_mode}")

def read_serial():
    result = subprocess.run(['nrfjprog', '--memrd', f'0x{UICR_CUSTOMER_BASE:08X}', '--n', '16'],
                           capture_output=True, text=True)
    if result.returncode != 0:
        if check_low_voltage(result.stderr):
            print("ERROR: Low voltage detected on device")
            sys.exit(1)
        elif 'readback protection' in result.stderr.lower() or 'ap-protection' in result.stderr.lower():
            print("Readback protection detected, recovering device...")
            recover_result = subprocess.run(['nrfjprog', '--recover'], capture_output=True, text=True)
            if recover_result.returncode != 0:
                if check_low_voltage(recover_result.stderr):
                    print("ERROR: Low voltage detected on device")
                else:
                    print(f"ERROR: Recovery failed: {recover_result.stderr}")
                sys.exit(1)
            result = subprocess.run(['nrfjprog', '--memrd', f'0x{UICR_CUSTOMER_BASE:08X}', '--n', '16'],
                                   capture_output=True, text=True)
            if result.returncode != 0:
                if check_low_voltage(result.stderr):
                    print("ERROR: Low voltage detected on device")
                else:
                    print("ERROR: No device detected")
                sys.exit(1)
        else:
            print("ERROR: No device detected")
            sys.exit(1)

    lines = result.stdout.strip().split('\n')

    all_values = []
    for line in lines:
        hex_line = line.split(':')[1].strip().split('|')[0].strip()
        all_values.extend(hex_line.split())

    ccc = int(all_values[0], 16)
    ppp = int(all_values[1], 16)
    ssssss = int(all_values[2], 16)
    print(f"Serial: {ccc:03d}-{ppp:03d}-{ssssss:06d}")

    debug_mode = 'not set'
    if len(all_values) > 3:
        debug_val = int(all_values[3], 16)
        if debug_val != 0xFFFFFFFF:
            debug_mode = bool(debug_val)
    print(f"Debug mode: {debug_mode}")

def flash_firmware(firmware_version=None):
    mcuboot_hex = 'mcuboot/mcuboot.hex'
    if firmware_version:
        app_hex = f'firmware/{firmware_version}.zephyr.signed.hex'
    else:
        app_hex = 'firmware/v0.1.31.zephyr.signed.hex'

    if not os.path.exists(mcuboot_hex):
        print(f"Error: MCUboot hex not found: {mcuboot_hex}")
        sys.exit(1)
    if not os.path.exists(app_hex):
        print(f"Error: App hex not found: {app_hex}")
        sys.exit(1)

    print("Flashing MCUboot bootloader...")
    result = subprocess.run(['nrfjprog', '--program', mcuboot_hex, '--sectorerase', '--verify'],
                          capture_output=True, text=True)
    if result.returncode != 0:
        if check_low_voltage(result.stderr):
            print("ERROR: Low voltage detected on device")
        else:
            print(f"Error programming MCUboot: {result.stderr}")
        sys.exit(1)

    print("Flashing signed application...")
    result = subprocess.run(['nrfjprog', '--program', app_hex, '--sectorerase', '--verify', '--reset'],
                          capture_output=True, text=True)
    if result.returncode != 0:
        if check_low_voltage(result.stderr):
            print("ERROR: Low voltage detected on device")
        else:
            print(f"Error programming app: {result.stderr}")
        sys.exit(1)

    # subprocess.run(['nrfjprog', '--reset'], capture_output=True)
    print("Firmware flashed successfully")

def erase():
    subprocess.run(['nrfjprog', '--eraseall'])
    print("Erased")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage:")
        print("  python serial_prog.py erase")
        print("  python serial_prog.py raw")
        print("  python serial_prog.py write 001-002-000123 [debug_mode: 0/1]")
        print("  python serial_prog.py read")
        print("  python serial_prog.py flash")
        sys.exit(1)

    if sys.argv[1] == 'erase':
        erase()
    elif sys.argv[1] == 'raw':
        read_raw()
    elif sys.argv[1] == 'write':
        debug_mode = bool(int(sys.argv[3])) if len(sys.argv) > 3 else None
        write_serial(sys.argv[2], debug_mode)
    elif sys.argv[1] == 'read':
        read_serial()
    elif sys.argv[1] == 'flash':
        firmware_version = sys.argv[2] if len(sys.argv) > 2 else None
        flash_firmware(firmware_version)