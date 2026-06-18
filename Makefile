# Simple kernel build

K = kernel
FS ?= legacy

ifeq ($(FS),fat16)
CONFIG_FS_FAT16 := 1
else ifeq ($(FS),legacy)
CONFIG_FS_FAT16 := 0
else
$(error unknown FS=$(FS), expected legacy or fat16)
endif

BUILD = build/$(FS)
KERNEL = $(BUILD)/kernel.elf
KERNEL_ASM = $(BUILD)/kernel.asm
FSIMG = $(BUILD)/fs.img

KINCLUDE = $(K)/include
KARCH = $(K)/arch/riscv
KCORE = $(K)/core
KDRV = $(K)/drivers
KLIB = $(K)/lib
KLD = $(K)/ld
KFS = $(K)/fs

CPUS ?= 4
RAM ?= 128M
QEMU_TEST_TIMEOUT ?= 60
LAB4_PUBLIC_TEST_CMDS = lab4test\n

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
	-kernel $(KERNEL) \
	-m $(RAM) \
	-smp $(CPUS) \
	-nographic \
	-global virtio-mmio.force-legacy=false
QEMUFSOPTS = -drive file=$(FSIMG),if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0

QEMUGDB = -S -gdb tcp::26000

# Optional extra --add NAME=PATH arguments for mkxv6fs (e.g. lab test fixtures)
FSIMG_EXTRA_ADD ?=

# C flags
CFLAGS = -Wall -O -fno-omit-frame-pointer -ggdb
CFLAGS += -mcmodel=medany -mno-relax
CFLAGS += -ffreestanding -fno-common -nostdlib
CFLAGS += -fno-pie -no-pie
CFLAGS += -MMD -MP
CFLAGS += -I$(KINCLUDE)

# Debug logger level: 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR, 4=NONE
LOG_LEVEL ?= 3
CFLAGS += -DLOG_LEVEL=$(LOG_LEVEL)
CFLAGS += -DCONFIG_FS_FAT16=$(CONFIG_FS_FAT16)

UCFLAGS = -Wall -O -fno-omit-frame-pointer -ggdb
UCFLAGS += -mcmodel=medany -mno-relax
UCFLAGS += -ffreestanding -fno-common -nostdlib
UCFLAGS += -fno-pie -no-pie
UCFLAGS += -MMD -MP
UCFLAGS += -I$(KINCLUDE) -I$(U)
UCFLAGS += -DCONFIG_FS_FAT16=$(CONFIG_FS_FAT16)
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

ifeq ($(FS),fat16)
SRCS += \
	$(KFS)/fat16fs.c \
	$(KFS)/bptree.c
else
SRCS += \
	$(KFS)/legacyfs.c
endif

KOBJS = $(patsubst %.c,$(BUILD)/%.o,$(filter %.c,$(SRCS)))
KOBJS += $(patsubst %.S,$(BUILD)/%.o,$(filter %.S,$(SRCS)))

UPROGS = \
	init \
	sh \
	hello \
	quiet \
	stressio \
	stsched \
	stressd \
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
	kwset \
	kwget \
	query \
	bonusbench \
	lab4test \
	rscr \

UCOMMON = \
	$(UBUILD)/entry.o \
	$(UBUILD)/syscall.o \
	$(UBUILD)/ulib.o \
	$(UBUILD)/printf.o
UOBJS = $(UCOMMON) $(patsubst %,$(UBUILD)/%.o,$(UPROGS))
UELFS = $(patsubst %,$(UBUILD)/%.elf,$(UPROGS))

# Default target
all: $(KERNEL) $(COMDB)

# Build rules
$(BUILD)/%.o: %.c
	@mkdir -p $(@D) $(COMDBDIR)
	COMPDB_DIR=$(COMDBDIR) COMPDB_FILE=$< $(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.S
	@mkdir -p $(@D) $(COMDBDIR)
	COMPDB_DIR=$(COMDBDIR) COMPDB_FILE=$< $(CC) $(CFLAGS) -c -o $@ $<

# Link kernel. Each filesystem variant writes its own kernel under build/<fs>/
$(KERNEL): $(KLD)/kernel.ld $(KOBJS) | $(COMDB)
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) -T $(KLD)/kernel.ld -o $@ $(KOBJS)

# Compatibility aliases: do not emit stale top-level artifacts.
# The real files are build/legacy/kernel.elf or build/fat16/kernel.elf.
kernel.elf: $(KERNEL)
	@echo "kernel: $(KERNEL)"

fs.img: $(FSIMG)
	@echo "fsimg: $(FSIMG)"

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
qemu: $(KERNEL) $(COMDB) fsimg
	$(QEMU) $(QEMUOPTS) $(QEMUFSOPTS)

qemu-check: $(KERNEL) $(COMDB) fsimg
	@mkdir -p $(BUILD)
	@python3 tools/run_qemu_until.py \
		--timeout "$(QEMU_TEST_TIMEOUT)" \
		--log "$(BUILD)/qemu-check.log" \
		--success "shell:.*->" \
		--fail "\\[PANIC\\]" \
		--fail "panic:" \
		--fail "kerneltrap" \
		-- $(QEMU) $(QEMUOPTS) $(QEMUFSOPTS)
	@printf "qemu boot check passed; log: %s\n" "$(BUILD)/qemu-check.log"

fs: $(KERNEL) $(COMDB) fsimg
	$(QEMU) $(QEMUOPTS) $(QEMUFSOPTS)

FSIMG_ADDS = \
	$(FSIMG_EXTRA_ADD) \
	--add test.sh=user/test.sh \
	--add init=$(UBUILD)/init.elf \
	--add sh=$(UBUILD)/sh.elf \
	--add hello=$(UBUILD)/hello.elf \
	--add quiet=$(UBUILD)/quiet.elf \
	--add stressio=$(UBUILD)/stressio.elf \
	--add stsched=$(UBUILD)/stsched.elf \
	--add stressd=$(UBUILD)/stressd.elf \
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
	--add kwset=$(UBUILD)/kwset.elf \
	--add kwget=$(UBUILD)/kwget.elf \
	--add query=$(UBUILD)/query.elf \
	--add bonusbench=$(UBUILD)/bonusbench.elf \
	--add bonus.sh=user/bonus.sh \
	--add lab4test=$(UBUILD)/lab4test.elf \
	--add rscr=$(UBUILD)/rscr.elf \

ifeq ($(FS),fat16)
fsimg: $(FSIMG)

$(FSIMG): $(UELFS) tools/fat16_pack.py
	@mkdir -p $(@D)
	rm -f $@
	python3 tools/fat16_pack.py --image $@ --create-size-mib 32 $(FSIMG_ADDS)
else
fsimg: $(FSIMG)

$(FSIMG): $(UELFS) tools/mkfsimg.py
	@mkdir -p $(@D)
	rm -f $@
	python3 tools/mkfsimg.py \
		--image $@ \
		--size-blocks 65536 \
		$(FSIMG_ADDS)
endif

qemu-gdb: $(KERNEL) $(COMDB) fsimg
	$(QEMU) $(QEMUOPTS) $(QEMUFSOPTS) $(QEMUGDB)

gdb: $(KERNEL) fsimg
	$(QEMU) $(QEMUOPTS) $(QEMUFSOPTS) $(QEMUGDB) & \
	$(GDB) $(KERNEL) -ex "target remote localhost:26000"

$(KERNEL_ASM): $(KERNEL)
	$(OBJDUMP) -d $(KERNEL) > $@

kernel.asm: $(KERNEL_ASM)

$(COMDB): $(KOBJS)
	@mkdir -p $(BUILD)
	@python3 tools/merge_compdb.py $(COMDBDIR) $(COMDB)

compdb: $(COMDB)

lab4-public-test:
	$(MAKE) FS=fat16 _lab4-public-test

_lab4-public-test: $(KERNEL) $(COMDB) fsimg
	@mkdir -p $(BUILD)
	@python3 tools/run_qemu_until.py \
		--timeout "$(QEMU_TEST_TIMEOUT)" \
		--log "$(BUILD)/lab4-public-test.log" \
		--commands "$(LAB4_PUBLIC_TEST_CMDS)" \
		--success "LAB4_PUBLIC_DONE" \
		--fail "\\[PANIC\\]" \
		--fail "panic:" \
		--fail "kerneltrap" \
		-- $(QEMU) $(QEMUOPTS) $(QEMUFSOPTS)
	@if ! grep -q "All Lab4 public tests passed" $(BUILD)/lab4-public-test.log || \
		grep -Eq "\\[FAIL\\]" $(BUILD)/lab4-public-test.log; then \
		grep -E "\\[PASS\\]|\\[FAIL\\]|All Lab4 public tests passed|Lab4 public tests failed|LAB4_PUBLIC_DONE" \
			$(BUILD)/lab4-public-test.log || true; \
		exit 1; \
	fi
	@printf "lab4 public tests passed; log: %s\n" "$(BUILD)/lab4-public-test.log"

clean:
	rm -rf build kernel.elf kernel.asm fs.img

.PHONY: all qemu qemu-check fs fsimg qemu-gdb gdb clean compdb lab4-public-test _lab4-public-test legacy fat16 kernel.elf kernel.asm fs.img

legacy:
	$(MAKE) FS=legacy all fsimg

fat16:
	$(MAKE) FS=fat16 all fsimg

-include $(KOBJS:.o=.d) $(UOBJS:.o=.d)
