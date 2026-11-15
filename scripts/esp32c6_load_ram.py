#!/usr/bin/env python3
"""
ZenRTOS ESP32-C6 Direct-to-SRAM Loader & Serial Monitor (Zero external dependencies, uses pyserial)
Speaks the native Espressif ROM SLIP bootloader protocol (SYNC, MEM_BEGIN, MEM_DATA, MEM_END)
over the built-in USB-Serial/JTAG port (/dev/ttyACM1).
"""
import struct
import sys
import time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM1"
BIN_PATH = sys.argv[2] if len(sys.argv) > 2 else "build/zen_rtos_esp32c6.bin"
LOAD_ADDR = 0x40800000
ENTRY_ADDR = 0x40800008  # Skip 8-byte Direct-Boot magic header to _start

ESP_MEM_BEGIN = 0x05
ESP_MEM_END   = 0x06
ESP_MEM_DATA  = 0x07
ESP_SYNC      = 0x08
ESP_CHECKSUM_MAGIC = 0xEF
BLOCK_SIZE = 1024


def slip_encode(pkt: bytes) -> bytes:
    out = bytearray([0xC0])
    for b in pkt:
        if b == 0xDB:
            out.extend([0xDB, 0xDD])
        elif b == 0xC0:
            out.extend([0xDB, 0xDC])
        else:
            out.append(b)
    out.append(0xC0)
    return bytes(out)


def slip_read_packet(ser: serial.Serial, timeout: float = 1.0) -> bytes:
    t0 = time.time()
    in_pkt = False
    escape = False
    buf = bytearray()
    while time.time() - t0 < timeout:
        b = ser.read(1)
        if not b:
            continue
        c = b[0]
        if not in_pkt:
            if c == 0xC0:
                in_pkt = True
                buf.clear()
            continue
        if escape:
            if c == 0xDC:
                buf.append(0xC0)
            elif c == 0xDD:
                buf.append(0xDB)
            else:
                buf.append(c)
            escape = False
        elif c == 0xDB:
            escape = True
        elif c == 0xC0:
            if len(buf) > 0:
                return bytes(buf)
        else:
            buf.append(c)
    return b""


def send_cmd(ser: serial.Serial, op: int, data: bytes, chk: int = 0, timeout: float = 1.0) -> bool:
    pkt = struct.pack("<BBHI", 0x00, op, len(data), chk) + data
    ser.write(slip_encode(pkt))
    t0 = time.time()
    while time.time() - t0 < timeout:
        resp = slip_read_packet(ser, timeout=0.3)
        if len(resp) >= 8 and resp[0] == 0x01 and resp[1] == op:
            status = resp[8] if len(resp) > 8 else 0
            return status == 0
    return False


def enter_download_mode(ser: serial.Serial):
    # Standard ESP32-C6 USB-Serial/JTAG reset into ROM Download Mode
    ser.dtr = False
    ser.rts = False
    time.sleep(0.05)
    ser.dtr = True
    ser.rts = False
    time.sleep(0.05)
    ser.rts = True
    ser.dtr = False
    time.sleep(0.05)
    ser.dtr = True
    time.sleep(0.05)
    ser.rts = False
    ser.dtr = False
    time.sleep(0.15)
    ser.reset_input_buffer()


def main():
    with open(BIN_PATH, "rb") as f:
        image = f.read()
    # Pad image to 4-byte boundary
    if len(image) % 4 != 0:
        image += b"\x00" * (4 - (len(image) % 4))

    print(f"[esp32c6] Connecting to {PORT}...")
    ser = serial.Serial(PORT, 115200, timeout=0.05)
    enter_download_mode(ser)

    sync_payload = b"\x07\x07\x12\x20" + (b"\x55" * 32)
    synced = False
    for _ in range(10):
        if send_cmd(ser, ESP_SYNC, sync_payload, 0, timeout=0.3):
            synced = True
            break
    if not synced:
        print("[esp32c6] ERROR: Failed to sync with ESP32-C6 ROM bootloader")
        sys.exit(1)
    # Drain remaining sync responses
    time.sleep(0.05)
    ser.reset_input_buffer()
    print("[esp32c6] Synced with ESP32-C6 ROM bootloader!")

    num_blocks = (len(image) + BLOCK_SIZE - 1) // BLOCK_SIZE
    print(f"[esp32c6] Uploading {len(image)} bytes ({num_blocks} blocks) to SRAM 0x{LOAD_ADDR:08X}...")
    mem_begin = struct.pack("<IIII", len(image), num_blocks, BLOCK_SIZE, LOAD_ADDR)
    if not send_cmd(ser, ESP_MEM_BEGIN, mem_begin, 0, timeout=1.0):
        print("[esp32c6] ERROR: MEM_BEGIN rejected")
        sys.exit(1)

    for seq in range(num_blocks):
        chunk = image[seq * BLOCK_SIZE : (seq + 1) * BLOCK_SIZE]
        chk = ESP_CHECKSUM_MAGIC
        for b in chunk:
            chk ^= b
        hdr = struct.pack("<IIII", len(chunk), seq, 0, 0)
        if not send_cmd(ser, ESP_MEM_DATA, hdr + chunk, chk, timeout=1.0):
            print(f"[esp32c6] ERROR: MEM_DATA block {seq} failed")
            sys.exit(1)

    print(f"[esp32c6] Starting ZenRTOS at entry 0x{ENTRY_ADDR:08X} (_start)...")
    mem_end = struct.pack("<II", 0, ENTRY_ADDR)
    send_cmd(ser, ESP_MEM_END, mem_end, 0, timeout=0.3)

    if "--monitor" in sys.argv:
        print("[esp32c6] Monitoring serial output (2.5s)...")
        t0 = time.time()
        out = bytearray()
        while time.time() - t0 < 2.5:
            data = ser.read(256)
            if data:
                out.extend(data)
        print(out.decode("utf-8", errors="replace"))
    ser.close()


if __name__ == "__main__":
    main()
