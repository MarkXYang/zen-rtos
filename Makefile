# ZenRTOS Makefile
# Target: STM32L476RG (ARM Cortex-M4F), ESP32-C6-Pico (RISC-V RV32IMAC) & Linux Host Test Suite

ZC            ?= /home/mark/code/Zen-C/zc
CLANG         ?= clang
LLVM_OBJCOPY  ?= llvm-objcopy

CC_HOST       ?= gcc
CFLAGS_HOST   ?= -Wall -Wextra -O2 -Ikernel/include -DRTOS_HOST_TEST

# ARM Target Flags (Clang bare-metal)
TARGET_TRIPLE ?= arm-none-eabi
CPU           ?= cortex-m4
CFLAGS_ARM    ?= --target=$(TARGET_TRIPLE) -mcpu=$(CPU) -mthumb \
                 -mfloat-abi=soft -O2 -Wall -Wextra -ffreestanding \
                 -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
                 -DZC_FUNC= -DZC_GLOBAL= \
                 -Ikernel/include -Ibsp/stm32l476 -Iarch/arm/cortex-m4 \
                 -fdata-sections -ffunction-sections

LDFLAGS_ARM   ?= --target=$(TARGET_TRIPLE) -mcpu=$(CPU) -mthumb \
                 -nostdlib -fuse-ld=lld \
                 -Wl,-T,bsp/stm32l476/link.ld \
                 -Wl,--gc-sections

# ESP32-C6-Pico Target Flags (Clang bare-metal RISC-V RV32IMAC)
CFLAGS_ESP32C6  ?= --target=riscv32-unknown-elf -march=rv32imac -mabi=ilp32 \
                   -mcmodel=medany -O2 -Wall -Wextra -ffreestanding \
                   -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
                   -DRTOS_ARCH_RISCV -DZC_FUNC= -DZC_GLOBAL= \
                   -Ikernel/include -Ibsp/esp32c6 -Iarch/riscv/rv32 \
                   -fdata-sections -ffunction-sections

LDFLAGS_ESP32C6 ?= --target=riscv32-unknown-elf -march=rv32imac -mabi=ilp32 \
                   -nostdlib -fuse-ld=lld \
                   -Wl,-T,bsp/esp32c6/link.ld \
                   -Wl,--gc-sections

BUILD_DIR     := build

.PHONY: all build esp32c6 test zenc flash flash-esp32c6 shell shell-esp32c6 clean

all: build

# ----------------------------------------------------------------------
# Target 1: Host Unit Tests (Runs natively on Linux)
# ----------------------------------------------------------------------
test: $(BUILD_DIR)/test_scheduler
	@echo "--- Executing ZenRTOS Host Test Suite ---"
	@./$(BUILD_DIR)/test_scheduler

$(BUILD_DIR)/test_scheduler: tests/host/test_scheduler.c kernel/rtos.c
	@mkdir -p $(BUILD_DIR)
	$(CC_HOST) $(CFLAGS_HOST) $^ -o $@

# ----------------------------------------------------------------------
# Target 2: Zen-C Freestanding Transpilation (app/main.zc -> app/main_zc.c)
# ----------------------------------------------------------------------
zenc: app/main_zc.c

app/main_zc.c: app/main.zc kernel/include/rtos.h bsp/stm32l476/stm32l476.h bsp/esp32c6/esp32c6.h
	@echo "--- Transpiling Zen-C (--freestanding) to C ---"
	$(ZC) transpile --freestanding app/main.zc -Ikernel/include -Ibsp/stm32l476 -o app/main_zc.c

# ----------------------------------------------------------------------
# Target 3: STM32L476 Bare-Metal Binary Build (directly from app/main.zc!)
# ----------------------------------------------------------------------
build: $(BUILD_DIR)/zen_rtos.bin $(BUILD_DIR)/zen_rtos.elf
	@echo "--- STM32L476 ZenRTOS Build Complete ---"
	@ls -lh $(BUILD_DIR)/zen_rtos.elf $(BUILD_DIR)/zen_rtos.bin

$(BUILD_DIR)/zen_rtos.elf: kernel/rtos.c \
                           arch/arm/cortex-m4/context.s \
                           bsp/stm32l476/startup.s \
                           app/main_zc.c
	@mkdir -p $(BUILD_DIR)
	$(CLANG) $(CFLAGS_ARM) $(LDFLAGS_ARM) $^ -o $@

$(BUILD_DIR)/zen_rtos.bin: $(BUILD_DIR)/zen_rtos.elf
	$(LLVM_OBJCOPY) -O binary $< $@

# ----------------------------------------------------------------------
# Target 4: ESP32-C6-Pico Bare-Metal RISC-V Build (directly from app/main.zc!)
# ----------------------------------------------------------------------
esp32c6: $(BUILD_DIR)/zen_rtos_esp32c6.bin $(BUILD_DIR)/zen_rtos_esp32c6.elf
	@echo "--- ESP32-C6-Pico (RISC-V RV32IMAC) ZenRTOS Build Complete ---"
	@ls -lh $(BUILD_DIR)/zen_rtos_esp32c6.elf $(BUILD_DIR)/zen_rtos_esp32c6.bin

$(BUILD_DIR)/zen_rtos_esp32c6.elf: kernel/rtos.c \
                                   arch/riscv/rv32/context.s \
                                   bsp/esp32c6/startup.s \
                                   app/main_zc.c
	@mkdir -p $(BUILD_DIR)
	$(CLANG) $(CFLAGS_ESP32C6) $(LDFLAGS_ESP32C6) $^ -o $@

$(BUILD_DIR)/zen_rtos_esp32c6.bin: $(BUILD_DIR)/zen_rtos_esp32c6.elf
	$(LLVM_OBJCOPY) -O binary $< $@

flash: build
	openocd -f interface/stlink.cfg -f target/stm32l4x.cfg \
		-c "program $(BUILD_DIR)/zen_rtos.elf verify reset exit"

flash-esp32c6: esp32c6
	python3 scripts/esp32c6_load.py /dev/ttyACM1 $(BUILD_DIR)/zen_rtos_esp32c6.bin

shell:
	python3 -m serial.tools.miniterm --eol CR /dev/ttyACM0 115200

shell-esp32c6:
	python3 -m serial.tools.miniterm --eol CR /dev/ttyACM1 115200

clean:
	rm -rf $(BUILD_DIR) app/main_zc.c
