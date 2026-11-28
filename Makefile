# ZenRTOS Makefile
# Targets: STM32L476RG (ARM Cortex-M4F), ESP32-C6-Pico (RISC-V RV32IMAC),
#          NXP i.MX RT1021 (ARM Cortex-M7) & Linux Host Test Suite

ZC            ?= /home/mark/code/Zen-C/zc
CLANG         ?= clang
LLVM_OBJCOPY  ?= llvm-objcopy

CC_HOST       ?= gcc
CFLAGS_HOST   ?= -Wall -Wextra -O2 -Ikernel/include -Ibsp/esp32c6 -DRTOS_HOST_TEST \
                 -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
                 -DZC_FUNC= -DZC_GLOBAL=

# ARM Target Flags (Clang bare-metal)
TARGET_TRIPLE ?= arm-none-eabi
CPU           ?= cortex-m4
CFLAGS_ARM    ?= --target=$(TARGET_TRIPLE) -mcpu=$(CPU) -mthumb \
                 -mfloat-abi=soft -O2 -Wall -Wextra -ffreestanding \
                 -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
                 -Wno-parentheses-equality \
                 -DZC_FUNC= -DZC_GLOBAL= \
                 -Ikernel/include -Ibsp/stm32l476 -Iarch/arm/cortex-m4 \
                 -fdata-sections -ffunction-sections

LDFLAGS_ARM   ?= --target=$(TARGET_TRIPLE) -mcpu=$(CPU) -mthumb \
                 -nostdlib -fuse-ld=lld \
                 -Wl,-T,bsp/stm32l476/link.ld \
                 -Wl,--gc-sections

# ESP32-C6-Pico Target Flags (Clang bare-metal RISC-V RV32IMAC)
CFLAGS_ESP32C6  ?= --target=riscv32-unknown-elf -march=rv32imac_zicsr_zifencei -mabi=ilp32 \
                   -mcmodel=medany -O2 -Wall -Wextra -ffreestanding \
                   -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
                   -Wno-parentheses-equality \
                   -DRTOS_ARCH_RISCV -DZC_FUNC= -DZC_GLOBAL= \
                   -Ikernel/include -Ibsp/esp32c6 -Iarch/riscv/rv32 \
                   -fdata-sections -ffunction-sections

LDFLAGS_ESP32C6 ?= --target=riscv32-unknown-elf -march=rv32imac_zicsr_zifencei -mabi=ilp32 \
                   -nostdlib -fuse-ld=lld \
                   -Wl,-T,bsp/esp32c6/link.ld \
                   -Wl,--gc-sections

# ESP32-C6-Pico Wi-Fi Build Flags (Direct-Boot Flash XIP + Espressif Wi-Fi/PHY blobs)
ESP32C6_WIFI_DIR := third_party/esp32c6_wifi
CFLAGS_ESP32C6_WIFI ?= --target=riscv32-unknown-elf -march=rv32imac_zicsr_zifencei -mabi=ilp32 \
                       -mcmodel=medany -O2 -Wall -Wextra -ffreestanding \
                       -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
                       -Wno-parentheses-equality \
                       -DRTOS_ARCH_RISCV -DZC_FUNC= -DZC_GLOBAL= \
                       -Ikernel/include -Ibsp/esp32c6 -I$(ESP32C6_WIFI_DIR)/include -Iarch/riscv/rv32 \
                       -fdata-sections -ffunction-sections

LDFLAGS_ESP32C6_WIFI ?= --target=riscv32-unknown-elf -march=rv32imac_zicsr_zifencei -mabi=ilp32 \
                        -nostdlib -fuse-ld=lld \
                        -Wl,-T,bsp/esp32c6/link_wifi.ld \
                        -Wl,-T,$(ESP32C6_WIFI_DIR)/ld/esp32c6.rom.ld \
                        -Wl,-T,$(ESP32C6_WIFI_DIR)/ld/esp32c6.rom.api.ld \
                        -Wl,-T,$(ESP32C6_WIFI_DIR)/ld/esp32c6.rom.libgcc.ld \
                        -Wl,-T,$(ESP32C6_WIFI_DIR)/ld/esp32c6.rom.phy.ld \
                        -Wl,-T,$(ESP32C6_WIFI_DIR)/ld/esp32c6.rom.coexist.ld \
                        -Wl,-T,$(ESP32C6_WIFI_DIR)/ld/esp32c6.rom.net80211.ld \
                        -Wl,-T,$(ESP32C6_WIFI_DIR)/ld/esp32c6.rom.pp.ld \
                        -Wl,-T,$(ESP32C6_WIFI_DIR)/ld/esp32c6.rom.systimer.ld \
                        -Wl,--gc-sections

ESP32C6_WIFI_LIBS := -Wl,--start-group \
                     $(ESP32C6_WIFI_DIR)/lib/libnet80211.a \
                     $(ESP32C6_WIFI_DIR)/lib/libpp.a \
                     $(ESP32C6_WIFI_DIR)/lib/libcore.a \
                     $(ESP32C6_WIFI_DIR)/lib/libcoexist.a \
                     $(ESP32C6_WIFI_DIR)/lib/libphy.a \
                     $(ESP32C6_WIFI_DIR)/lib/libbtbb.a \
                     -Wl,--end-group

# NXP i.MX RT1021 Target Flags (Clang bare-metal ARM Cortex-M7)
# Set RT1021_BOOT=ram for OCRAM/DTCM RAM-load bring-up, or xip (default) for FlexSPI QSPI NOR
RT1021_BOOT     ?= xip
RT1021_LDSCRIPT := $(if $(filter ram,$(RT1021_BOOT)),bsp/rt1021/link_ram.ld,bsp/rt1021/link_xip.ld)
RT1021_BOOT_SRC := $(if $(filter ram,$(RT1021_BOOT)),,bsp/rt1021/boot_hdr.c)

CFLAGS_RT1021   ?= --target=$(TARGET_TRIPLE) -mcpu=cortex-m7 -mthumb \
                   -mfloat-abi=soft -O2 -Wall -Wextra -ffreestanding \
                   -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
                   -Wno-parentheses-equality \
                   -DZC_FUNC= -DZC_GLOBAL= \
                   -Ikernel/include -Ibsp/rt1021 -Iarch/arm/cortex-m7 \
                   -fdata-sections -ffunction-sections

LDFLAGS_RT1021  ?= --target=$(TARGET_TRIPLE) -mcpu=cortex-m7 -mthumb \
                   -nostdlib -fuse-ld=lld \
                   -Wl,-T,$(RT1021_LDSCRIPT) \
                   -Wl,--gc-sections

BUILD_DIR     := build

.PHONY: all build esp32c6 esp32c6-wifi rt1021 test zenc flash flash-esp32c6 flash-esp32c6-wifi flash-rt1021 shell shell-esp32c6 shell-rt1021 clean

all: build

# ----------------------------------------------------------------------
# Target 1: Host Unit Tests (Runs natively on Linux)
# ----------------------------------------------------------------------
test: $(BUILD_DIR)/test_scheduler
	@echo "--- Executing ZenRTOS Host Test Suite ---"
	@./$(BUILD_DIR)/test_scheduler

$(BUILD_DIR)/test_scheduler: tests/host/test_scheduler.c kernel/rtos.c $(BUILD_DIR)/esp32c6_wpa2_crypto_zc.c
	@mkdir -p $(BUILD_DIR)
	$(CC_HOST) $(CFLAGS_HOST) $^ -o $@

# ----------------------------------------------------------------------
# Target 2: Zen-C Freestanding Transpilation (app/main.zc + BSP .zc)
# ----------------------------------------------------------------------
zenc: app/main_zc.c $(BUILD_DIR)/stm32l476_zc.c $(BUILD_DIR)/esp32c6_zc.c $(BUILD_DIR)/rt1021_zc.c

app/main_zc.c: app/main.zc kernel/include/rtos.h
	@echo "--- Transpiling Zen-C Application (app/main.zc) to C ---"
	$(ZC) transpile --freestanding app/main.zc -Ikernel/include -o app/main_zc.c

$(BUILD_DIR)/stm32l476_zc.c: bsp/stm32l476/stm32l476.zc kernel/include/rtos.h
	@mkdir -p $(BUILD_DIR)
	@echo "--- Transpiling Zen-C STM32L476 BSP (bsp/stm32l476/stm32l476.zc) to C ---"
	$(ZC) transpile --freestanding bsp/stm32l476/stm32l476.zc -Ikernel/include -o $(BUILD_DIR)/stm32l476_zc.c

$(BUILD_DIR)/esp32c6_zc.c: bsp/esp32c6/esp32c6.zc kernel/include/rtos.h
	@mkdir -p $(BUILD_DIR)
	@echo "--- Transpiling Zen-C ESP32-C6 BSP (bsp/esp32c6/esp32c6.zc) to C ---"
	$(ZC) transpile --freestanding bsp/esp32c6/esp32c6.zc -Ikernel/include -o $(BUILD_DIR)/esp32c6_zc.c

$(BUILD_DIR)/esp32c6_wifi_zc.c: bsp/esp32c6/wifi.zc kernel/include/rtos.h
	@mkdir -p $(BUILD_DIR)
	@echo "--- Transpiling Zen-C ESP32-C6 Wi-Fi Module (bsp/esp32c6/wifi.zc) to C ---"
	$(ZC) transpile --freestanding bsp/esp32c6/wifi.zc -Ikernel/include -o $(BUILD_DIR)/esp32c6_wifi_zc.c

$(BUILD_DIR)/esp32c6_wpa2_crypto_zc.c: bsp/esp32c6/wpa2_crypto.zc
	@mkdir -p $(BUILD_DIR)
	@echo "--- Transpiling Zen-C WPA2 Crypto Primitives (bsp/esp32c6/wpa2_crypto.zc) to C ---"
	$(ZC) transpile --freestanding bsp/esp32c6/wpa2_crypto.zc -Ikernel/include -o $(BUILD_DIR)/esp32c6_wpa2_crypto_zc.c

$(BUILD_DIR)/esp32c6_wifi_wpa2_zc.c: bsp/esp32c6/wifi_wpa2.zc
	@mkdir -p $(BUILD_DIR)
	@echo "--- Transpiling Zen-C ESP32-C6 WPA2 Supplicant (bsp/esp32c6/wifi_wpa2.zc) to C ---"
	$(ZC) transpile --freestanding bsp/esp32c6/wifi_wpa2.zc -Ikernel/include -o $(BUILD_DIR)/esp32c6_wifi_wpa2_zc.c

$(BUILD_DIR)/rt1021_zc.c: bsp/rt1021/rt1021.zc kernel/include/rtos.h
	@mkdir -p $(BUILD_DIR)
	@echo "--- Transpiling Zen-C NXP RT1021 BSP (bsp/rt1021/rt1021.zc) to C ---"
	$(ZC) transpile --freestanding bsp/rt1021/rt1021.zc -Ikernel/include -o $(BUILD_DIR)/rt1021_zc.c

# ----------------------------------------------------------------------
# Target 3: STM32L476 Bare-Metal Binary Build (from app/main.zc + stm32l476.zc)
# ----------------------------------------------------------------------
build: $(BUILD_DIR)/zen_rtos.bin $(BUILD_DIR)/zen_rtos.elf
	@echo "--- STM32L476 ZenRTOS Build Complete ---"
	@ls -lh $(BUILD_DIR)/zen_rtos.elf $(BUILD_DIR)/zen_rtos.bin

$(BUILD_DIR)/zen_rtos.elf: kernel/rtos.c \
                           arch/arm/cortex-m4/context.s \
                           bsp/stm32l476/startup.s \
                           $(BUILD_DIR)/stm32l476_zc.c \
                           app/main_zc.c
	@mkdir -p $(BUILD_DIR)
	$(CLANG) $(CFLAGS_ARM) $(LDFLAGS_ARM) $^ -o $@

$(BUILD_DIR)/zen_rtos.bin: $(BUILD_DIR)/zen_rtos.elf
	$(LLVM_OBJCOPY) -O binary $< $@

# ----------------------------------------------------------------------
# Target 4: ESP32-C6-Pico Bare-Metal RISC-V Build (from app/main.zc + esp32c6.zc)
# ----------------------------------------------------------------------
esp32c6: $(BUILD_DIR)/zen_rtos_esp32c6.bin $(BUILD_DIR)/zen_rtos_esp32c6.elf
	@echo "--- ESP32-C6-Pico (RISC-V RV32IMAC) ZenRTOS Build Complete ---"
	@ls -lh $(BUILD_DIR)/zen_rtos_esp32c6.elf $(BUILD_DIR)/zen_rtos_esp32c6.bin

$(BUILD_DIR)/zen_rtos_esp32c6.elf: kernel/rtos.c \
                                   arch/riscv/rv32/context.s \
                                   bsp/esp32c6/startup.s \
                                   $(BUILD_DIR)/esp32c6_zc.c \
                                   app/main_zc.c
	@mkdir -p $(BUILD_DIR)
	$(CLANG) $(CFLAGS_ESP32C6) $(LDFLAGS_ESP32C6) $^ -o $@

$(BUILD_DIR)/zen_rtos_esp32c6.bin: $(BUILD_DIR)/zen_rtos_esp32c6.elf
	$(LLVM_OBJCOPY) -O binary $< $@

# ----------------------------------------------------------------------
# Target 4b: ESP32-C6-Pico Wi-Fi Build (Direct-Boot Flash XIP + Blobs)
# ----------------------------------------------------------------------
esp32c6-wifi: $(BUILD_DIR)/zen_rtos_esp32c6_wifi.bin $(BUILD_DIR)/zen_rtos_esp32c6_wifi.elf
	@echo "--- ESP32-C6-Pico Wi-Fi (Direct-Boot XIP) ZenRTOS Build Complete ---"
	@ls -lh $(BUILD_DIR)/zen_rtos_esp32c6_wifi.elf $(BUILD_DIR)/zen_rtos_esp32c6_wifi.bin

$(BUILD_DIR)/zen_rtos_esp32c6_wifi.elf: kernel/rtos.c \
                                        arch/riscv/rv32/context.s \
                                        bsp/esp32c6/startup.s \
                                        bsp/esp32c6/wifi_heap.c \
                                        bsp/esp32c6/wifi_libc.c \
                                        bsp/esp32c6/wifi_osi.c \
                                        bsp/esp32c6/link_wifi.ld \
                                        $(BUILD_DIR)/esp32c6_zc.c \
                                        $(BUILD_DIR)/esp32c6_wifi_zc.c \
                                        $(BUILD_DIR)/esp32c6_wpa2_crypto_zc.c \
                                        $(BUILD_DIR)/esp32c6_wifi_wpa2_zc.c \
                                        app/main_zc.c
	@if [ ! -f "$(ESP32C6_WIFI_DIR)/lib/libnet80211.a" ] || [ ! -f "$(ESP32C6_WIFI_DIR)/include/phy_init_data.c" ]; then \
		./scripts/fetch_esp32c6_wifi_blobs.sh; \
	fi
	@mkdir -p $(BUILD_DIR)
	$(CLANG) $(CFLAGS_ESP32C6_WIFI) $(LDFLAGS_ESP32C6_WIFI) $(filter-out %.ld,$^) $(ESP32C6_WIFI_DIR)/include/phy_init_data.c $(ESP32C6_WIFI_LIBS) -o $@

$(BUILD_DIR)/zen_rtos_esp32c6_wifi.bin: $(BUILD_DIR)/zen_rtos_esp32c6_wifi.elf
	$(LLVM_OBJCOPY) -O binary $< $@

# ----------------------------------------------------------------------
# Target 5: NXP i.MX RT1021 Bare-Metal Cortex-M7 Build (XIP or RAM)
# ----------------------------------------------------------------------
rt1021: $(BUILD_DIR)/zen_rtos_rt1021.bin $(BUILD_DIR)/zen_rtos_rt1021.elf
	@echo "--- NXP i.MX RT1021 (ARM Cortex-M7, boot=$(RT1021_BOOT)) ZenRTOS Build Complete ---"
	@ls -lh $(BUILD_DIR)/zen_rtos_rt1021.elf $(BUILD_DIR)/zen_rtos_rt1021.bin

$(BUILD_DIR)/zen_rtos_rt1021.elf: kernel/rtos.c \
                                  arch/arm/cortex-m7/context.s \
                                  bsp/rt1021/startup.s \
                                  $(RT1021_BOOT_SRC) \
                                  $(BUILD_DIR)/rt1021_zc.c \
                                  app/main_zc.c \
                                  $(RT1021_LDSCRIPT)
	@mkdir -p $(BUILD_DIR)
	$(CLANG) $(CFLAGS_RT1021) $(LDFLAGS_RT1021) $(filter-out %.ld,$^) -o $@

$(BUILD_DIR)/zen_rtos_rt1021.bin: $(BUILD_DIR)/zen_rtos_rt1021.elf
	$(LLVM_OBJCOPY) -O binary $< $@

flash: build
	openocd -f interface/stlink.cfg -f target/stm32l4x.cfg \
		-c "program $(BUILD_DIR)/zen_rtos.elf verify reset exit"

flash-esp32c6: esp32c6
	python3 scripts/esp32c6_load.py /dev/ttyACM0 $(BUILD_DIR)/zen_rtos_esp32c6.bin

flash-esp32c6-wifi: esp32c6-wifi
	python3 scripts/esp32c6_load.py /dev/ttyACM0 $(BUILD_DIR)/zen_rtos_esp32c6_wifi.bin --raw-flash

flash-rt1021: rt1021
	@if [ "$(RT1021_BOOT)" = "ram" ]; then \
		openocd -f bsp/rt1021/openocd.cfg \
			-c "init; reset halt; load_image $(BUILD_DIR)/zen_rtos_rt1021.elf; reg sp [mrw 0x20200000]; reg pc [mrw 0x20200004]; resume; exit"; \
	else \
		openocd -f bsp/rt1021/openocd.cfg \
			-c "program $(BUILD_DIR)/zen_rtos_rt1021.elf verify reset exit"; \
	fi

shell:
	python3 -m serial.tools.miniterm --eol CR /dev/ttyACM0 115200

shell-esp32c6:
	python3 -m serial.tools.miniterm --eol CR /dev/ttyACM0 115200

shell-rt1021:
	python3 -m serial.tools.miniterm --eol CR /dev/ttyACM0 115200

clean:
	rm -rf $(BUILD_DIR) app/main_zc.c
