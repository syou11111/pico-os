#!/usr/bin/env python3
"""Host terminal and file transfer utility for PicoOS."""

import argparse
import ctypes
import msvcrt
import os
import shutil
import sys
import threading
import time
from pathlib import Path

import serial
import serial.tools.list_ports

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_UF2 = PROJECT_ROOT / "build" / "pico_os.uf2"
MAX_FILE_BYTES = 4032

EXT_KEY_MAP = {
    b"H": b"\x1b[A",   # Up
    b"P": b"\x1b[B",   # Down
    b"M": b"\x1b[C",   # Right
    b"K": b"\x1b[D",   # Left
    b"G": b"\x1b[H",   # Home
    b"O": b"\x1b[F",   # End
    b"S": b"\x1b[3~",  # Delete
}


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def detect_pico_port(explicit_port: str | None = None) -> str:
    if explicit_port:
        return explicit_port
    for p in serial.tools.list_ports.comports():
        hwid = (p.hwid or "").upper()
        if "2E8A" in hwid or p.vid == 0x2E8A:
            return p.device
    return "COM4"


def open_serial(port_name: str) -> serial.Serial:
    ser = serial.Serial(port_name, baudrate=115200, timeout=0.05)
    ser.dtr = True
    ser.rts = True
    time.sleep(0.15)
    return ser


def enable_vt_mode() -> None:
    if os.name != "nt":
        return
    try:
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
        mode = ctypes.c_uint32()
        if kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            kernel32.SetConsoleMode(handle, mode.value | 0x0004)
    except Exception:
        pass


def find_rp2_drive() -> str | None:
    if os.name != "nt":
        return None
    kernel32 = ctypes.windll.kernel32
    vol_buf = ctypes.create_unicode_buffer(256)
    fs_buf = ctypes.create_unicode_buffer(256)
    bitmask = kernel32.GetLogicalDrives()
    for idx in range(26):
        if not (bitmask & (1 << idx)):
            continue
        drive = f"{chr(65 + idx)}:\\"
        if kernel32.GetDriveTypeW(drive) != 2:  # DRIVE_REMOVABLE
            continue
        ok = kernel32.GetVolumeInformationW(
            drive, vol_buf, 256, None, None, None, fs_buf, 256
        )
        if ok and vol_buf.value.upper() == "RPI-RP2":
            return drive
    return None


def read_line_until(ser: serial.Serial, timeout_s: float = 3.0) -> str | None:
    deadline = time.monotonic() + timeout_s
    buf = bytearray()
    while time.monotonic() < deadline:
        ch = ser.read(1)
        if not ch:
            continue
        if ch == b"\n":
            return buf.decode("ascii", errors="replace").strip()
        if ch != b"\r":
            buf.extend(ch)
    return None


def send_file(ser: serial.Serial, local_path: str, remote_name: str | None = None) -> bool:
    p = Path(local_path)
    if not p.is_file():
        print(f"Error: local file not found: {local_path}")
        return False

    data = p.read_bytes()
    if len(data) > MAX_FILE_BYTES:
        print(f"Error: file too large ({len(data)} > {MAX_FILE_BYTES} bytes)")
        return False

    if not remote_name:
        remote_name = p.name

    crc = crc16_ccitt(data)
    ser.reset_input_buffer()
    ser.write(f"rx {remote_name}\r".encode("ascii"))
    ser.flush()

    ready = False
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        line = read_line_until(ser, timeout_s=1.0)
        if line is None:
            continue
        if line.startswith("@READY"):
            ready = True
            break
        if line.startswith("@ERR"):
            print(f"PicoOS error: {line}")
            return False

    if not ready:
        print("Error: timed out waiting for @READY from PicoOS")
        return False

    ser.write(f"@BEGIN {len(data)} {crc:04X}\r\n".encode("ascii"))
    hex_str = data.hex().upper()
    for i in range(0, len(hex_str), 64):
        chunk = hex_str[i : i + 64] + "\r\n"
        ser.write(chunk.encode("ascii"))
        ser.flush()
        time.sleep(0.002)
    ser.write(b"@END\r\n")
    ser.flush()

    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        line = read_line_until(ser, timeout_s=1.0)
        if line is None:
            continue
        if line.startswith("@OK"):
            print(f"Uploaded {p} -> {remote_name} ({len(data)} bytes, CRC {crc:04X})")
            return True
        if line.startswith("@ERR"):
            print(f"Upload failed: {line}")
            return False

    print("Error: timed out waiting for @OK confirmation")
    return False


def recv_file(ser: serial.Serial, remote_name: str, local_path: str | None = None) -> bool:
    if not local_path:
        local_path = Path(remote_name).name

    ser.reset_input_buffer()
    ser.write(f"tx {remote_name}\r".encode("ascii"))
    ser.flush()

    expected_size = None
    expected_crc = None
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        line = read_line_until(ser, timeout_s=1.0)
        if line is None:
            continue
        if line.startswith("@ERR"):
            print(f"PicoOS error: {line}")
            return False
        if line.startswith("@BEGIN "):
            parts = line.split()
            if len(parts) < 4:
                print(f"Malformed @BEGIN header: {line}")
                return False
            expected_size = int(parts[2], 10)
            expected_crc = int(parts[3], 16)
            break

    if expected_size is None or expected_crc is None:
        print("Error: timed out waiting for @BEGIN from PicoOS")
        return False

    hex_chars: list[str] = []
    saw_end = False
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline:
        line = read_line_until(ser, timeout_s=1.5)
        if line is None:
            continue
        if line.startswith("@END"):
            saw_end = True
            break
        if line.startswith("@ERR"):
            print(f"Download error: {line}")
            return False
        hex_chars.append(line.strip())

    if not saw_end:
        print("Error: timed out waiting for @END")
        return False

    try:
        data = bytes.fromhex("".join(hex_chars))
    except ValueError as exc:
        print(f"Error: invalid hex payload: {exc}")
        return False

    if len(data) != expected_size:
        print(f"Error: size mismatch ({len(data)} != {expected_size})")
        return False

    actual_crc = crc16_ccitt(data)
    if actual_crc != expected_crc:
        print(f"Error: CRC mismatch ({actual_crc:04X} != {expected_crc:04X})")
        return False

    out_path = Path(local_path)
    out_path.write_bytes(data)
    print(f"Downloaded {remote_name} -> {out_path} ({len(data)} bytes, CRC {actual_crc:04X})")
    return True


def flash_uf2(ser: serial.Serial | None = None, port_name: str | None = None, uf2_path: Path = DEFAULT_UF2) -> str | None:
    if not uf2_path.is_file():
        print(f"Error: UF2 not found at {uf2_path}")
        return port_name

    drive = find_rp2_drive()
    if drive is None:
        target_port = port_name or detect_pico_port()
        try:
            if ser is not None and ser.is_open:
                ser.write(b"\rbootsel\r")
                ser.flush()
                time.sleep(0.2)
                ser.close()
            else:
                tmp = open_serial(target_port)
                tmp.write(b"\rbootsel\r")
                tmp.flush()
                time.sleep(0.2)
                tmp.close()
            print(f"Sent 'bootsel' to {target_port}. Waiting for RPI-RP2...")
        except Exception as exc:
            print(f"Could not send bootsel via {target_port}: {exc}")
            print("Waiting for RPI-RP2 (press BOOTSEL if needed)...")

        deadline = time.monotonic() + 12.0
        while time.monotonic() < deadline:
            drive = find_rp2_drive()
            if drive is not None:
                break
            time.sleep(0.25)

    if drive is None:
        print("Error: RPI-RP2 drive did not appear.")
        return None

    dest = Path(drive) / uf2_path.name
    shutil.copy2(uf2_path, dest)
    print(f"Flashed {uf2_path.name} -> {dest}. Waiting for PicoOS reboot...")

    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline:
        for p in serial.tools.list_ports.comports():
            hwid = (p.hwid or "").upper()
            if "2E8A" in hwid or p.vid == 0x2E8A:
                time.sleep(0.4)
                return p.device
        time.sleep(0.25)

    return port_name or "COM4"


def run_reader(ser: serial.Serial, stop_event: threading.Event) -> None:
    stdout_buf = sys.stdout.buffer
    while not stop_event.is_set():
        try:
            data = ser.read(256)
        except Exception:
            break
        if data:
            stdout_buf.write(data)
            stdout_buf.flush()


def prompt_line(prompt: str) -> str:
    sys.stdout.write(prompt)
    sys.stdout.flush()
    return sys.stdin.readline().strip()


def interactive_term(port_arg: str | None) -> int:
    enable_vt_mode()
    port_name = detect_pico_port(port_arg)
    try:
        ser = open_serial(port_name)
    except Exception as exc:
        print(f"Failed to open {port_name}: {exc}")
        return 1

    print(f"Connected to PicoOS on {port_name}")
    print("Hotkeys: Ctrl+T = File Transfer / Flash Menu, Ctrl+] = Exit\r\n")

    stop_event = threading.Event()
    reader = threading.Thread(target=run_reader, args=(ser, stop_event), daemon=True)
    reader.start()

    try:
        while True:
            if not msvcrt.kbhit():
                time.sleep(0.01)
                continue

            ch = msvcrt.getch()
            if ch in (b"\x00", b"\xe0"):
                ext = msvcrt.getch()
                mapped = EXT_KEY_MAP.get(ext)
                if mapped:
                    ser.write(mapped)
                    ser.flush()
                continue

            if ch == b"\x1d":  # Ctrl+]
                break

            if ch == b"\x14":  # Ctrl+T
                stop_event.set()
                reader.join(timeout=0.5)

                print("\r\n--- PicoOS Host Menu ---")
                print("  [p] Upload file (put)")
                print("  [g] Download file (get)")
                print("  [f] Flash build/pico_os.uf2")
                print("  [c] Cancel")
                choice = prompt_line("Select [p/g/f/c]: ").lower()

                if choice == "p":
                    local_p = prompt_line("Local path: ")
                    remote_p = prompt_line("Remote name (Enter=same): ")
                    if local_p:
                        send_file(ser, local_p, remote_p or None)
                elif choice == "g":
                    remote_p = prompt_line("Remote name: ")
                    local_p = prompt_line("Local path (Enter=same): ")
                    if remote_p:
                        recv_file(ser, remote_p, local_p or None)
                elif choice == "f":
                    new_port = flash_uf2(ser=ser, port_name=port_name)
                    if new_port:
                        port_name = new_port
                        ser = open_serial(port_name)
                        print(f"Reconnected to {port_name}")
                    else:
                        print("Could not reconnect automatically.")
                        break
                else:
                    print("Cancelled.")

                ser.write(b"\r")
                ser.flush()
                stop_event.clear()
                reader = threading.Thread(target=run_reader, args=(ser, stop_event), daemon=True)
                reader.start()
                continue

            ser.write(ch)
            ser.flush()
    finally:
        stop_event.set()
        reader.join(timeout=0.3)
        if ser.is_open:
            ser.close()
        print("\r\nDisconnected.")

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="PicoOS serial terminal & file transfer tool")
    parser.add_argument("--port", help="Serial COM port (auto-detected if omitted)")
    sub = parser.add_subparsers(dest="cmd")

    sub.add_parser("term", help="Open interactive VT100 terminal (Ctrl+T menu, Ctrl+] exit)")

    p_put = sub.add_parser("put", help="Upload a local file to PicoFS")
    p_put.add_argument("local_path", help="Local source file path")
    p_put.add_argument("remote_name", nargs="?", help="Destination filename in PicoFS")

    p_get = sub.add_parser("get", help="Download a file from PicoFS")
    p_get.add_argument("remote_name", help="Source filename in PicoFS")
    p_get.add_argument("local_path", nargs="?", help="Destination local path")

    p_flash = sub.add_parser("flash", help="Reboot Pico into BOOTSEL and flash UF2")
    p_flash.add_argument("uf2", nargs="?", default=str(DEFAULT_UF2), help="Path to UF2 file")

    args = parser.parse_args()
    cmd = args.cmd or "term"

    if cmd == "term":
        return interactive_term(args.port)

    if cmd == "flash":
        res = flash_uf2(port_name=args.port, uf2_path=Path(args.uf2))
        return 0 if res else 1

    port_name = detect_pico_port(args.port)
    ser = open_serial(port_name)
    try:
        if cmd == "put":
            ok = send_file(ser, args.local_path, args.remote_name)
            return 0 if ok else 1
        if cmd == "get":
            ok = recv_file(ser, args.remote_name, args.local_path)
            return 0 if ok else 1
    finally:
        if ser.is_open:
            ser.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
