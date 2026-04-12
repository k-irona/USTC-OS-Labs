# Simple kernel build

K = kernel
BUILD = build

KINCLUDE = $(K)/include
KARCH = $(K)/arch/riscv
KCORE = $(K)/core
KDRV = $(K)/drivers
KLIB = $(K)/lib
KLD = $(K)/ld
KFS = $(K)/fs

CPUS ?= 4
RAM ?= 128M

U = user
UBUILD = $(BUILD)/user

# Toolchain auto-detect
ifndef TOOLPREFIX
TOOLPREFIX := $(shell if riscv64-unknown-elf-gcc -print-libgcc-file-name > /dev/null 2>&1; \
	then echo "riscv64-unknown-elf-"; \
	elif riscv64-linux-gnu-gcc -print-libgcc-file-name > /dev/null 2>&1; \
	then echo "riscv64-linux-gnu-"; \
	else echo "***"; fi)
endif

ifeq ($(TOOLPREFIX),***)
$(error "Couldn't find a RISC-V toolchain in PATH")
endif

REALCC = $(TOOLPREFIX)gcc
CC = python3 tools/ccwrap.py $(REALCC)
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump
GDB = $(TOOLPREFIX)gdb

# QEMU auto-detect
ifndef QEMU
QEMU := $(shell if which qemu-system-riscv64 > /dev/null; \
	then echo qemu-system-riscv64; \
	else echo "***"; fi)
endif

ifeq ($(QEMU),***)
$(error "qemu-system-riscv64 not found in PATH")
endif

QEMUOPTS = -machine virt \
	-bios none \
	-kernel kernel.elf \
	-m $(RAM) \
	-smp $(CPUS) \
	-nographic \
	-global virtio-mmio.force-legacy=false
QEMUFSOPTS = -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0

QEMUGDB = -S -gdb tcp::26000

# Optional extra --add NAME=PATH arguments for mkxv6fs (e.g. lab test fixtures)
FSIMG_EXTRA_ADD ?=

# C flags
CFLAGS = -Wall  -O -fno-omit-frame-pointer -ggdb
CFLAGS += -mcmodel=medany -mno-relax
CFLAGS += -ffreestanding -fno-common -nostdlib
CFLAGS += -fno-pie -no-pie
CFLAGS += -MMD -MP
CFLAGS += -I$(KINCLUDE)

# Debug logger level: 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR, 4=NONE
LOG_LEVEL ?= 3
CFLAGS += -DLOG_LEVEL=$(LOG_LEVEL)

UCFLAGS = -Wall -O -fno-omit-frame-pointer -ggdb
UCFLAGS += -mcmodel=medany -mno-relax
UCFLAGS += -ffreestanding -fno-common -nostdlib
UCFLAGS += -fno-pie -no-pie
UCFLAGS += -MMD -MP
UCFLAGS += -I$(KINCLUDE) -I$(U)
ULDFLAGS = -Wl,--build-id=none

LDFLAGS = -z max-page-size=4096 
COMDBDIR = $(BUILD)/compdb
COMDB = $(BUILD)/compile_commands.json

# Kernel sources
SRCS = \
	$(KARCH)/entry.S \
	$(KARCH)/kernelvec.S \
	$(KARCH)/trampoline.S \
	$(KARCH)/timervec.S \
	$(KARCH)/swtch.S \
	$(KARCH)/start.c \
	$(KDRV)/plic.c \
	$(KDRV)/uart.c \
	$(KDRV)/virtio_disk.c \
	$(KLIB)/string.c \
	$(KLIB)/printf.c \
	$(KCORE)/trap.c \
	$(KCORE)/cpu.c \
	$(KCORE)/intr.c \
	$(KCORE)/spinlock.c \
	$(KCORE)/sleeplock.c \
	$(KCORE)/kalloc.c \
	$(KCORE)/vm.c \
	$(KCORE)/console.c \
	$(KCORE)/file.c \
	$(KCORE)/proc.c \
	$(KCORE)/main.c \
	$(KCORE)/syscall.c \
	$(KCORE)/pipe.c \
	$(KCORE)/bio.c \
	$(KDRV)/gpu.c \
	$(KFS)/fs.c \

KOBJS = $(patsubst %.c,$(BUILD)/%.o,$(filter %.c,$(SRCS)))
KOBJS += $(patsubst %.S,$(BUILD)/%.o,$(filter %.S,$(SRCS)))

UPROGS = \
	init \
	sh \
	hello \
	quiet \
	stressio \
	stsched \
	stressdisk \
	pid \
	uptime \
	sleep \
	killer \
	kill \
	pingpong \
	fstat \
	forktest \
	zombie \
	echo \
	cat \
	wc \
	grep \
	ls \
	find \
	xargs \
	fstest \
	mkdir \
	rm \
	ln \
	touch \
	runscript \
	rs_status \
	gpudemo \
	gpu_stats 
UCOMMON = \
	$(UBUILD)/entry.o \
	$(UBUILD)/syscall.o \
	$(UBUILD)/ulib.o \
	$(UBUILD)/printf.o
UOBJS = $(UCOMMON) $(patsubst %,$(UBUILD)/%.o,$(UPROGS))
UELFS = $(patsubst %,$(UBUILD)/%.elf,$(UPROGS))

# Default target
all: kernel.elf $(COMDB)

# Build rules
$(BUILD)/%.o: %.c
	@mkdir -p $(@D) $(COMDBDIR)
	COMPDB_DIR=$(COMDBDIR) COMPDB_FILE=$< $(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.S
	@mkdir -p $(@D) $(COMDBDIR)
	COMPDB_DIR=$(COMDBDIR) COMPDB_FILE=$< $(CC) $(CFLAGS) -c -o $@ $<

# Link kernel
kernel.elf: $(KLD)/kernel.ld $(KOBJS) | $(COMDB)
	$(LD) $(LDFLAGS) -T $(KLD)/kernel.ld -o $@ $(KOBJS)

# ------------------------------------------------------------
# User programs
# ------------------------------------------------------------

$(UBUILD)/%.o: $(U)/%.c
	@mkdir -p $(@D)
	$(REALCC) $(UCFLAGS) -c -o $@ $<

$(UBUILD)/%.o: $(U)/%.S
	@mkdir -p $(@D)
	$(REALCC) $(UCFLAGS) -c -o $@ $<

$(UBUILD)/%.elf: $(UCOMMON) $(UBUILD)/%.o $(U)/user.ld
	$(REALCC) $(UCFLAGS) $(ULDFLAGS) -T $(U)/user.ld -o $@ $(UCOMMON) $(UBUILD)/$*.o

# Run in QEMU
qemu: kernel.elf $(COMDB) fsimg
	$(QEMU) $(QEMUOPTS) $(QEMUFSOPTS)

fs: kernel.elf $(COMDB) fsimg
	$(QEMU) $(QEMUOPTS) $(QEMUFSOPTS)

fsimg: $(UELFS) tools/mkfsimg.py
	python3 tools/mkfsimg.py \
		--image fs.img \
		--size-blocks 65536 \
		$(FSIMG_EXTRA_ADD) \
		--add test.sh=user/test.sh \
		--add init=$(UBUILD)/init.elf \
		--add sh=$(UBUILD)/sh.elf \
		--add hello=$(UBUILD)/hello.elf \
		--add quiet=$(UBUILD)/quiet.elf \
		--add stressio=$(UBUILD)/stressio.elf \
		--add stsched=$(UBUILD)/stsched.elf \
		--add stressdisk=$(UBUILD)/stressdisk.elf \
		--add pid=$(UBUILD)/pid.elf \
		--add uptime=$(UBUILD)/uptime.elf \
		--add sleep=$(UBUILD)/sleep.elf \
		--add killer=$(UBUILD)/killer.elf \
		--add kill=$(UBUILD)/kill.elf \
		--add pingpong=$(UBUILD)/pingpong.elf \
		--add fstat=$(UBUILD)/fstat.elf \
		--add forktest=$(UBUILD)/forktest.elf \
		--add zombie=$(UBUILD)/zombie.elf \
		--add echo=$(UBUILD)/echo.elf \
		--add cat=$(UBUILD)/cat.elf \
		--add wc=$(UBUILD)/wc.elf \
		--add grep=$(UBUILD)/grep.elf \
		--add ls=$(UBUILD)/ls.elf \
		--add find=$(UBUILD)/find.elf \
		--add xargs=$(UBUILD)/xargs.elf \
		--add fstest=$(UBUILD)/fstest.elf \
		--add mkdir=$(UBUILD)/mkdir.elf \
		--add rm=$(UBUILD)/rm.elf \
		--add ln=$(UBUILD)/ln.elf \
		--add touch=$(UBUILD)/touch.elf \
		--add runscript=$(UBUILD)/runscript.elf \
		--add rs_status=$(UBUILD)/rs_status.elf \
		--add gpudemo=$(UBUILD)/gpudemo.elf \
		--add gpu_stats=$(UBUILD)/gpu_stats.elf 

qemu-gdb: kernel.elf $(COMDB) fsimg
	$(QEMU) $(QEMUOPTS) $(QEMUFSOPTS) $(QEMUGDB)

gdb: kernel.elf fsimg
	$(QEMU) $(QEMUOPTS) $(QEMUFSOPTS) $(QEMUGDB) & \
	$(GDB) kernel.elf -ex "target remote localhost:26000"

kernel.asm: kernel.elf
	$(OBJDUMP) -d kernel.elf > kernel.asm

$(COMDB): $(KOBJS)
	@mkdir -p $(BUILD)
	@python3 tools/merge_compdb.py $(COMDBDIR) $(COMDB)

compdb: $(COMDB)

clean:
	rm -rf $(BUILD) kernel.elf kernel.asm fs.img

.PHONY: all qemu fs fsimg qemu-gdb gdb clean compdb

-include $(KOBJS:.o=.d) $(UOBJS:.o=.d)
