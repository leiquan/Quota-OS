# 一条命令：make
# 产物：build/quotaos.elf / build/quotaos.bin（串口镜像）
# 构建可逐字节复现：逐个文件编译到 build/，不留临时文件名、构建路径和 build-id。
CROSS ?= aarch64-linux-gnu-
RUNTIME_LIB ?= -lgcc
CC := $(CROSS)gcc
RUNTIME_LIB ?= -lgcc

CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O2 \
	-ffreestanding -fno-stack-protector -fno-builtin -fno-pic -fno-pie \
	-mgeneral-regs-only -mstrict-align -fno-asynchronous-unwind-tables \
	-ffile-prefix-map=$(CURDIR)/= -I os/include -I build
LDFLAGS := -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none

# 启动横幅里的提交号：构建时的 git rev-parse --short=7 HEAD。
# 每个版本的提交连同 prebuilt/ 一起提交，所以横幅显示的是这个版本所基于的提交（它的父提交）。
# 映像里不放时间戳。
SRC_COMMIT := $(shell git rev-parse --short=7 HEAD 2>/dev/null || echo unknown)

SRCS := $(shell cat os/kernel/sources.list)
OBJS := $(patsubst %,build/%.o,$(basename $(SRCS)))

.PHONY: all clean run run-small

# The default prebuilt image contains LVGL. This target builds the serial kernel.
all: build/quotaos.elf build/quotaos.bin

# 提交号变了才重写这个文件，main.c 依赖它。
build/commit.h: FORCE
	@mkdir -p build
	@echo '#define QUOTAOS_COMMIT "$(SRC_COMMIT)"' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

build/kernel/main.o: build/commit.h

build/user/paint.elf: os/user/paint.S os/user/user.ld
	@mkdir -p build/user
	$(CC) -nostdlib -nostartfiles -static -no-pie -Wl,--build-id=none -T os/user/user.ld -o $@ os/user/paint.S

build/user/paint.bin: build/user/paint.elf
	$(CROSS)objcopy -O binary $< $@

build/kernel/proc.o: build/user/paint.bin

.PHONY: FORCE
FORCE:

build/%.o: os/%.c os/include/quotaos.h Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: os/%.S Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

build/quotaos.elf: $(OBJS) os/boot/linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -T os/boot/linker.ld -o $@ $(OBJS) $(RUNTIME_LIB)

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
		-nographic -kernel build/quotaos.bin -append serial

run-small: build/quotaos.bin
	qemu-system-aarch64 -machine virt,gic-version=2 -cpu cortex-a72 -m 64M \
		-nographic -kernel build/quotaos.bin -append serial

clean:
	rm -rf build
