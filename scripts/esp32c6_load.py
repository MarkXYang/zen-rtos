#!/usr/bin/env python3
"""
ZenRTOS ESP32-C6 SPI Flash & SRAM Programmer (Zero external dependencies, uses pyserial + hashlib)
Speaks the native Espressif ROM SLIP bootloader protocol over /dev/ttyACM1:
  - Default mode: Wraps build/zen_rtos_esp32c6.bin in a bootable ESP32-C6 (chip_id=0x000D)
    0xE9 Flash Image Header + SHA256 digest, programs it to non-volatile SPI Flash at 0x0000,
    and triggers a hardware reset so ZenRTOS persists across power cycles!
  - --ram mode: Uploads directly to HP SRAM at 0x40800000 and jumps to _start (0x40800008).
"""
import hashlib
import struct
import sys
import time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM1"
BIN_PATH = sys.argv[2] if len(sys.argv) > 2 else "build/zen_rtos_esp32c6.bin"
LOAD_ADDR = 0x40800000
ENTRY_ADDR = 0x40800008  # Skip 8-byte Direct-Boot magic header to _start

ESP_FLASH_BEGIN    = 0x02
ESP_FLASH_DATA     = 0x03
ESP_FLASH_END      = 0x04
ESP_MEM_BEGIN      = 0x05
ESP_MEM_END        = 0x06
ESP_MEM_DATA       = 0x07
ESP_SYNC           = 0x08
ESP_SPI_SET_PARAMS = 0x0B
ESP_SPI_ATTACH     = 0x0D

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


def send_cmd(ser: serial.Serial, op: int, data: bytes, chk: int = 0, timeout: float = 2.0) -> bool:
    pkt = struct.pack("<BBHI", 0x00, op, len(data), chk) + data
    ser.write(slip_encode(pkt))
    t0 = time.time()
    while time.time() - t0 < timeout:
        resp = slip_read_packet(ser, timeout=0.5)
        if len(resp) >= 8 and resp[0] == 0x01 and resp[1] == op:
            status = resp[8] if len(resp) > 8 else 0
            return status == 0
    return False


def enter_download_mode(ser: serial.Serial):
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


def hard_reset_to_flash(ser: serial.Serial):
    # Toggle RTS (EN=0 -> EN=1) with DTR=False (IO9=1) to boot from SPI Flash
    ser.dtr = False
    ser.rts = True
    time.sleep(0.05)
    ser.rts = False
    time.sleep(0.05)


def build_esp32c6_flash_image(raw_bin: bytes) -> bytes:
    """
    Wraps raw_bin in an official Espressif ESP32-C6 (chip_id=0x000D) bootable Flash image:
      - 24-byte esp_image_header_t (magic=0xE9, segments=1, DIO, 80MHz/4MB, entry=0x40800008, chip_id=0x000D, hash_appended=1)
      - 8-byte esp_image_segment_header_t (load_addr=0x40800000, data_len=len(raw_bin))
      - raw_bin payload
      - 16-byte alignment padding + 1-byte XOR checksum (0xEF ^ payload)
      - 32-byte SHA-256 digest
    """
    if len(raw_bin) % 4 != 0:
        raw_bin += b"\x00" * (4 - (len(raw_bin) % 4))

    # esp_image_header_t (24 bytes):
    # magic=0xE9, segment_count=1, spi_mode=2 (DIO), spi_speed_size=0x20 (4MB, DIV_2),
    # entry_addr=ENTRY_ADDR, wp_pin=0xEE, spi_pin_drv=0,0,0, chip_id=0x000D (ESP32-C6),
    # min_rev=0, min_rev_full=0, max_rev_full=0xFFFF, reserved=0,0,0,0, hash_appended=1
    img_hdr = struct.pack(
        "<BBBBIBBBBHBHHBBBBB",
        0xE9,               # magic
        1,                  # segment_count
        2,                  # spi_mode = DIO
        0x20,               # spi_size = 4MB (2 << 4) | spi_speed = DIV_2 (0)
        ENTRY_ADDR,         # entry_addr = 0x40800008 (_start)
        0xEE,               # wp_pin disabled
        0, 0, 0,            # spi_pin_drv
        0x000D,             # chip_id = ESP_CHIP_ID_ESP32C6
        0,                  # min_chip_rev
        0,                  # min_chip_rev_full
        0xFFFF,             # max_chip_rev_full
        0, 0, 0, 0,         # reserved
        1,                  # hash_appended = 1
    )
    seg_hdr = struct.pack("<II", LOAD_ADDR, len(raw_bin))

    chk = ESP_CHECKSUM_MAGIC
    for b in raw_bin:
        chk ^= b

    body = bytearray(img_hdr + seg_hdr + raw_bin)
    # Pad so that (len(body) + 1) is a multiple of 16 bytes
    pad_len = (16 - ((len(body) + 1) % 16)) % 16
    body.extend(b"\x00" * pad_len)
    body.append(chk)

    # Append 32-byte SHA-256 digest of the entire image so far
    sha256_digest = hashlib.sha256(body).digest()
    body.extend(sha256_digest)
    return bytes(body)


def main():
    with open(BIN_PATH, "rb") as f:
        raw_image = f.read()
    if len(raw_image) % 4 != 0:
        raw_image += b"\x00" * (4 - (len(raw_image) % 4))

    ram_only = "--ram" in sys.argv
    raw_flash = "--raw-flash" in sys.argv

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
    time.sleep(0.05)
    ser.reset_input_buffer()
    print("[esp32c6] Synced with ESP32-C6 ROM bootloader!")

    if ram_only:
        num_blocks = (len(raw_image) + BLOCK_SIZE - 1) // BLOCK_SIZE
        print(f"[esp32c6] Uploading {len(raw_image)} bytes ({num_blocks} blocks) to SRAM 0x{LOAD_ADDR:08X}...")
        mem_begin = struct.pack("<IIII", len(raw_image), num_blocks, BLOCK_SIZE, LOAD_ADDR)
        if not send_cmd(ser, ESP_MEM_BEGIN, mem_begin, 0, timeout=1.0):
            print("[esp32c6] ERROR: MEM_BEGIN rejected")
            sys.exit(1)
        for seq in range(num_blocks):
            chunk = raw_image[seq * BLOCK_SIZE : (seq + 1) * BLOCK_SIZE]
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
    else:
        flash_image = raw_image if raw_flash else build_esp32c6_flash_image(raw_image)
        num_blocks = (len(flash_image) + BLOCK_SIZE - 1) // BLOCK_SIZE
        mode_str = "Direct-Boot XIP (raw)" if raw_flash else "0xE9 SRAM image"

        print(f"[esp32c6] Attaching 4MB SPI Flash ({mode_str})...")
        if not send_cmd(ser, ESP_SPI_ATTACH, struct.pack("<II", 0, 0), 0, timeout=1.0):
            print("[esp32c6] ERROR: SPI_ATTACH failed")
            sys.exit(1)
        spi_params = struct.pack("<IIIIII", 0, 4 * 1024 * 1024, 64 * 1024, 4096, 256, 0xFFFF)
        send_cmd(ser, ESP_SPI_SET_PARAMS, spi_params, 0, timeout=1.0)

        erase_size = ((len(flash_image) + 4095) // 4096) * 4096
        print(f"[esp32c6] Erasing & programming {len(flash_image)} bytes ({num_blocks} blocks) to SPI Flash @ 0x00000000...")
        flash_begin = struct.pack("<IIIII", erase_size, num_blocks, BLOCK_SIZE, 0x00000000, 0)
        if not send_cmd(ser, ESP_FLASH_BEGIN, flash_begin, 0, timeout=15.0):
            print("[esp32c6] ERROR: FLASH_BEGIN rejected")
            sys.exit(1)

        for seq in range(num_blocks):
            chunk = flash_image[seq * BLOCK_SIZE : (seq + 1) * BLOCK_SIZE]
            if len(chunk) < BLOCK_SIZE:
                chunk += b"\xFF" * (BLOCK_SIZE - len(chunk))
            chk = ESP_CHECKSUM_MAGIC
            for b in chunk:
                chk ^= b
            hdr = struct.pack("<IIII", len(chunk), seq, 0, 0)
            if not send_cmd(ser, ESP_FLASH_DATA, hdr + chunk, chk, timeout=2.0):
                print(f"[esp32c6] ERROR: FLASH_DATA block {seq} failed")
                sys.exit(1)

        send_cmd(ser, ESP_FLASH_END, struct.pack("<I", 0), 0, timeout=0.5)
        hard_reset_to_flash(ser)
        print("[esp32c6] Successfully programmed SPI Flash @ 0x00000000 and booted ZenRTOS! (Persistent across power cycles)")

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
