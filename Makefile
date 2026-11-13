# 一条命令：make
# 产物：build/quotaos.elf / build/quotaos.bin，并复制到 prebuilt/
# 构建可逐字节复现：逐个文件编译到 build/，不留临时文件名、构建路径和 build-id。
CROSS ?= aarch64-linux-gnu-
CC := $(CROSS)gcc

CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O2 \
	-ffreestanding -fno-stack-protector -fno-builtin -fno-pic -fno-pie \
	-mgeneral-regs-only -mstrict-align -fno-asynchronous-unwind-tables \
	-ffile-prefix-map=$(CURDIR)/= -I include
LDFLAGS := -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none

SRCS := boot/startup.S boot/vectors.S \
	kernel/main.c kernel/uart.c kernel/printf.c kernel/string.c \
	kernel/dtb.c kernel/gic.c kernel/timer.c kernel/resource.c \
	kernel/sched.c kernel/demo.c
OBJS := $(patsubst %,build/%.o,$(basename $(SRCS)))

.PHONY: all clean run run-small

all: build/quotaos.elf build/quotaos.bin prebuilt/quotaos.elf prebuilt/quotaos.bin

build/%.o: %.c include/quotaos.h Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: %.S Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

build/quotaos.elf: $(OBJS) boot/linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -T boot/linker.ld -o $@ $(OBJS) -lgcc

build/quotaos.bin: build/quotaos.elf
	$(CROSS)objcopy -O binary $< $@

prebuilt/quotaos.elf: build/quotaos.elf
	@mkdir -p prebuilt
	cp $< $@

prebuilt/quotaos.bin: build/quotaos.bin
	@mkdir -p prebuilt
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
