#!/usr/bin/env bash
# Fetch pinned Espressif ESP32-C6 Wi-Fi & PHY prebuilt RISC-V static libraries
# and ESP32-C6 Mask ROM symbol linker scripts into third_party/esp32c6_wifi/.
set -euo pipefail

DEST_DIR="${1:-third_party/esp32c6_wifi}"
mkdir -p "${DEST_DIR}/lib" "${DEST_DIR}/ld" "${DEST_DIR}/include"

echo "=== Fetching ESP32-C6 Wi-Fi & PHY Static Libraries into ${DEST_DIR} ==="

WIFI_BASE="https://raw.githubusercontent.com/espressif/esp32-wifi-lib/master/esp32c6"
for lib in libnet80211.a libpp.a libcore.a; do
    echo "  -> ${lib}"
    curl -fsSL "${WIFI_BASE}/${lib}" -o "${DEST_DIR}/lib/${lib}"
done

COEX_BASE="https://raw.githubusercontent.com/espressif/esp-coex-lib/master/esp32c6"
echo "  -> libcoexist.a"
curl -fsSL "${COEX_BASE}/libcoexist.a" -o "${DEST_DIR}/lib/libcoexist.a"

PHY_BASE="https://raw.githubusercontent.com/espressif/esp-phy-lib/master/esp32c6"
for lib in libphy.a libbtbb.a; do
    echo "  -> ${lib}"
    curl -fsSL "${PHY_BASE}/${lib}" -o "${DEST_DIR}/lib/${lib}"
done

echo "=== Fetching ESP32-C6 Mask ROM Linker Scripts ==="
IDF_ROM_BASE="https://raw.githubusercontent.com/espressif/esp-idf/v5.3.2/components/esp_rom/esp32c6/ld"
for ld in esp32c6.rom.ld esp32c6.rom.api.ld esp32c6.rom.libgcc.ld esp32c6.rom.phy.ld esp32c6.rom.coexist.ld esp32c6.rom.net80211.ld esp32c6.rom.pp.ld esp32c6.rom.systimer.ld; do
    echo "  -> ${ld}"
    curl -fsSL "${IDF_ROM_BASE}/${ld}" -o "${DEST_DIR}/ld/${ld}"
done

echo "=== Fetching ESP-IDF Wi-Fi Headers ==="
IDF_WIFI_INC="https://raw.githubusercontent.com/espressif/esp-idf/v5.3.2/components/esp_wifi/include"
curl -fsSL "${IDF_WIFI_INC}/esp_private/wifi_os_adapter.h" -o "${DEST_DIR}/include/wifi_os_adapter.h"
curl -fsSL "${IDF_WIFI_INC}/esp_wifi.h" -o "${DEST_DIR}/include/esp_wifi.h"
curl -fsSL "${IDF_WIFI_INC}/esp_wifi_crypto_types.h" -o "${DEST_DIR}/include/esp_wifi_crypto_types.h"
curl -fsSL "${IDF_WIFI_INC}/esp_wifi_types_generic.h" -o "${DEST_DIR}/include/esp_wifi_types_generic.h"

echo "=== Done! Downloaded ESP32-C6 Wi-Fi blobs & ROM scripts to ${DEST_DIR} ==="
ls -lh "${DEST_DIR}/lib" "${DEST_DIR}/ld"
