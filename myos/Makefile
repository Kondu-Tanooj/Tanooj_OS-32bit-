# =============================================================================
# Makefile — MyOS build system
#
# Targets:
#   make          — build the kernel ELF binary
#   make run      — boot it in QEMU (no USB needed for testing)
#   make iso      — create a bootable .iso  (needs grub-mkrescue)
#   make usb      — flash the ISO to a USB drive  (DANGER: double-check DEVICE)
#   make clean    — remove build artefacts
#
# Prerequisites (Ubuntu/Debian):
#   sudo apt install nasm gcc-multilib grub-pc-bin xorriso qemu-system-x86
#
# Prerequisites (macOS — needs cross-compiler from OSDev wiki):
#   brew install nasm x86_64-elf-gcc x86_64-elf-binutils xorriso qemu
#   Then change CC to i686-elf-gcc and LD to i686-elf-ld below.
# =============================================================================

# --- Tools ---
# We use the HOST gcc but target i686 (32-bit x86).
# On a 64-bit host you need multilib support (gcc-multilib on Debian/Ubuntu).
# For a "proper" cross-compiler see: https://wiki.osdev.org/GCC_Cross-Compiler
AS       := nasm
CC       := gcc
LD       := ld

# --- Flags ---
ASFLAGS  := -f elf32

# -m32               : compile for 32-bit x86
# -ffreestanding     : no standard library or startup files
# -fno-stack-protector: stack canary needs libc — skip it
# -fno-pic           : no position-independent code
CFLAGS   := -m32 \
             -std=c11 \
             -ffreestanding \
             -fno-stack-protector \
             -fno-pic \
             -Wall -Wextra \
             -Iinclude

# -m elf_i386  : 32-bit ELF output
# -T linker.ld : use our memory layout script
# --nmagic     : no page alignment padding in output
LDFLAGS  := -m elf_i386 \
             -T linker.ld \
             --nmagic

# --- Sources & objects ---
BOOT_OBJ  := build/boot.o build/gdt_flush.o build/isr.o
KERN_OBJS := build/kernel.o build/terminal.o build/gdt.o build/idt.o build/keyboard.o build/pmm.o build/heap.o build/shell.o build/vfs.o build/ata.o build/tfs.o build/paging.o build/mouse.o build/editor.o build/process.o build/mount.o build/auth.o build/calc.o
OBJS      := $(BOOT_OBJ) $(KERN_OBJS)

KERNEL   := build/tanooj.elf
ISO      := build/tanooj.iso
DISK2    := build/disk2.img
DISK2_MB := 16

# =============================================================================
.PHONY: all run iso usb clean

all: $(KERNEL)
	@echo ""
	@echo "  Build successful -> $(KERNEL)"
	@echo "  Run 'make run' to boot TanoojOS in QEMU."
	@echo "  Run 'make iso' to create a bootable ISO."
	@echo ""

# --- Link ---
$(KERNEL): $(OBJS) linker.ld | build
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# --- Compile assembly ---
build/boot.o: boot/boot.asm | build
	$(AS) $(ASFLAGS) -o $@ $<

build/gdt_flush.o: boot/gdt_flush.asm | build
	$(AS) $(ASFLAGS) -o $@ $<

build/isr.o: boot/isr.asm | build
	$(AS) $(ASFLAGS) -o $@ $<

# --- Compile C sources ---
build/kernel.o: kernel/kernel.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/terminal.o: kernel/terminal.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/gdt.o: kernel/gdt.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/idt.o: kernel/idt.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/keyboard.o: kernel/keyboard.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/pmm.o: kernel/pmm.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/heap.o: kernel/heap.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/shell.o: kernel/shell.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/vfs.o: kernel/vfs.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/ata.o: kernel/ata.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/tfs.o: kernel/tfs.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/paging.o: kernel/paging.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/mouse.o: kernel/mouse.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/editor.o: kernel/editor.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/process.o: kernel/process.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/mount.o: kernel/mount.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/auth.o: kernel/auth.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/calc.o: kernel/calc.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

# --- Create build directory ---
build:
	mkdir -p build

# =============================================================================
# Run in QEMU (fastest way to test — no USB needed)
# =============================================================================
DISK     := build/disk.img
DISK_MB  := 4

# Create a blank disk image if it doesn't exist
$(DISK): | build
	dd if=/dev/zero of=$(DISK) bs=1M count=$(DISK_MB) status=none
	@echo "  Disk image created -> $(DISK) ($(DISK_MB) MiB)"

$(DISK2): | build
	dd if=/dev/zero of=$(DISK2) bs=1M count=$(DISK2_MB) status=none
	@echo "  Disk2 image created -> $(DISK2) ($(DISK2_MB) MiB)"

run: $(KERNEL) $(DISK)
	qemu-system-i386 		-kernel $(KERNEL) 		-m 32M 		-no-reboot 		-display sdl 		-drive file=$(DISK),format=raw,if=ide

# =============================================================================
# Run with a real SD card / USB drive as disk2
# Usage: make sdcard DEV=/dev/mmcblk0
#        make sdcard DEV=/dev/sdb
# Find your device with: lsblk
# WARNING: TanoojOS will write to this device!
# =============================================================================
sdcard: $(KERNEL) $(DISK)
ifndef DEV
	$(error "Set DEV to your SD/USB device, e.g.: make sdcard DEV=/dev/mmcblk0")
endif
	@echo "  Booting TanoojOS with real device: $(DEV)"
	@echo "  TanoojOS will mount it as /disk2"
	sudo qemu-system-i386 \
		-kernel $(KERNEL) \
		-m 32M \
		-no-reboot \
		-display sdl \
		-drive file=$(DISK),format=raw,if=ide,index=0 \
		-drive file=$(DEV),format=raw,if=ide,index=1

# =============================================================================
# Run the ISO in QEMU with proper full-screen VGA
run-iso: $(ISO)
	qemu-system-i386 \
		-cdrom $(ISO) \
		-m 32M \
		-no-reboot \
		-vga std \
		-display sdl \
		-drive file=$(DISK),format=raw,if=ide,index=0

# Debug mode: run with GDB server on port 1234
debug: $(KERNEL)
	qemu-system-i386 -kernel $(KERNEL) -s -S &
	gdb -ex "target remote :1234" -ex "symbol-file $(KERNEL)"

# =============================================================================
# Create a bootable ISO image
# Requires: grub-pc-bin, grub-common, xorriso
# =============================================================================
iso: $(KERNEL)
	mkdir -p build/iso/boot/grub
	cp $(KERNEL) build/iso/boot/tanooj.elf
	@echo 'set timeout=0'                       > build/iso/boot/grub/grub.cfg
	@echo 'set default=0'                       >> build/iso/boot/grub/grub.cfg
	@echo 'insmod vbe'                          >> build/iso/boot/grub/grub.cfg
	@echo 'insmod vga'                          >> build/iso/boot/grub/grub.cfg
	@echo 'set gfxmode=off'                     >> build/iso/boot/grub/grub.cfg
	@echo 'set gfxpayload=text'                 >> build/iso/boot/grub/grub.cfg
	@echo 'terminal_input console'              >> build/iso/boot/grub/grub.cfg
	@echo 'terminal_output console'             >> build/iso/boot/grub/grub.cfg
	@echo ''                                    >> build/iso/boot/grub/grub.cfg
	@echo 'menuentry "TanoojOS" {'              >> build/iso/boot/grub/grub.cfg
	@echo '    insmod multiboot'                >> build/iso/boot/grub/grub.cfg
	@echo '    multiboot /boot/tanooj.elf'      >> build/iso/boot/grub/grub.cfg
	@echo '    boot'                            >> build/iso/boot/grub/grub.cfg
	@echo '}'                                   >> build/iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) build/iso \
		--locales="" --themes=""
	@echo ""
	@echo "  ISO created -> $(ISO)"
	@echo "  Test with: qemu-system-i386 -cdrom $(ISO)"
	@echo "  Flash USB : sudo dd if=$(ISO) of=/dev/sdX bs=4M status=progress"
	@echo ""

# =============================================================================
# Flash to USB  *** DOUBLE-CHECK DEVICE — THIS WILL ERASE IT ***
# Usage: make usb DEVICE=/dev/sdb
# =============================================================================
usb: $(ISO)
ifndef DEVICE
	$(error "Set DEVICE, e.g.: make usb DEVICE=/dev/sdb")
endif
	@echo "WARNING: About to wipe $(DEVICE). Press Ctrl-C to cancel, Enter to continue."
	@read _
	sudo dd if=$(ISO) of=$(DEVICE) bs=4M status=progress
	sudo sync
	@echo "Done. Boot your laptop from $(DEVICE)."

# =============================================================================
clean:
	rm -rf build
