# ZenRTOS Makefile
# Target: STM32L476RG (ARM Cortex-M4F) & Linux Host Test Suite

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

BUILD_DIR     := build

.PHONY: all build test zenc clean

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

app/main_zc.c: app/main.zc kernel/include/rtos.h bsp/stm32l476/stm32l476.h
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

clean:
	rm -rf $(BUILD_DIR) app/main_zc.c
