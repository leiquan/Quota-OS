# 一条命令：make
# 产物：build/quotaos.elf，并复制到 prebuilt/quotaos.elf
CROSS ?= aarch64-linux-gnu-
CC := $(CROSS)gcc

CFLAGS := -std=gnu11 -Wall -Wextra -Wno-unused-function -O2 \
	-ffreestanding -fno-stack-protector -fno-builtin -fno-pic -fno-pie \
	-mgeneral-regs-only -mstrict-align -fno-asynchronous-unwind-tables \
	-I include
LDFLAGS := -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none

SRCS := boot/startup.S boot/vectors.S \
	kernel/main.c kernel/uart.c kernel/printf.c kernel/string.c \
	kernel/dtb.c kernel/gic.c kernel/timer.c kernel/resource.c \
	kernel/sched.c kernel/demo.c

.PHONY: all clean run run-small

all: build/quotaos.elf build/quotaos.bin prebuilt/quotaos.elf prebuilt/quotaos.bin

build/quotaos.elf: $(SRCS) boot/linker.ld include/quotaos.h
	mkdir -p build prebuilt
	$(CC) $(CFLAGS) $(LDFLAGS) -T boot/linker.ld -o $@ $(SRCS) -lgcc

build/quotaos.bin: build/quotaos.elf
	$(CROSS)objcopy -O binary $< $@

prebuilt/quotaos.elf: build/quotaos.elf
	cp $< $@

prebuilt/quotaos.bin: build/quotaos.bin
	cp $< $@

# 用原始映像启动，QEMU 才会按 Linux 引导协议把设备树指针放进 x0。
run: build/quotaos.bin
	qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a72 -m 128M \
		-nographic -kernel build/quotaos.bin

run-small: build/quotaos.bin
	qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a72 -m 64M \
		-nographic -kernel build/quotaos.bin

clean:
	rm -rf build
